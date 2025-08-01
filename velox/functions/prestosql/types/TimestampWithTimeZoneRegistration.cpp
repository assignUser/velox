/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/functions/prestosql/types/TimestampWithTimeZoneRegistration.h"

#include "velox/expression/CastExpr.h"
#include "velox/functions/lib/DateTimeFormatter.h"
#include "velox/functions/prestosql/types/TimestampWithTimeZoneType.h"
#include "velox/functions/prestosql/types/fuzzer_utils/TimestampWithTimeZoneInputGenerator.h"
#include "velox/type/tz/TimeZoneMap.h"

namespace facebook::velox {
namespace {
const tz::TimeZone* getTimeZoneFromConfig(const core::QueryConfig& config) {
  const auto sessionTzName = config.sessionTimezone();

  if (!sessionTzName.empty()) {
    return tz::locateZone(sessionTzName);
  }

  return tz::locateZone(0); // GMT
}

void castFromTimestamp(
    const SimpleVector<Timestamp>& inputVector,
    exec::EvalCtx& context,
    const SelectivityVector& rows,
    int64_t* rawResults) {
  const auto& config = context.execCtx()->queryCtx()->queryConfig();
  const auto* sessionTimeZone = getTimeZoneFromConfig(config);

  const auto adjustTimestampToTimezone = config.adjustTimestampToTimezone();

  context.applyToSelectedNoThrow(rows, [&](auto row) {
    auto ts = inputVector.valueAt(row);
    if (!adjustTimestampToTimezone) {
      // Treat TIMESTAMP as wall time in session time zone. This means that in
      // order to get its UTC representation we need to shift the value by the
      // offset of the time zone.
      ts.toGMT(*sessionTimeZone);
    }
    rawResults[row] = pack(ts.toMillis(), sessionTimeZone->id());
  });
}

void castFromDate(
    const SimpleVector<int32_t>& inputVector,
    exec::EvalCtx& context,
    const SelectivityVector& rows,
    int64_t* rawResults) {
  const auto& config = context.execCtx()->queryCtx()->queryConfig();
  const auto* sessionTimeZone = getTimeZoneFromConfig(config);

  static const int64_t kSecondsInDay = 86400;

  context.applyToSelectedNoThrow(rows, [&](auto row) {
    const auto days = inputVector.valueAt(row);
    Timestamp ts{days * kSecondsInDay, 0};
    if (sessionTimeZone != nullptr) {
      ts.toGMT(*sessionTimeZone);
    }
    rawResults[row] = pack(ts.toMillis(), sessionTimeZone->id());
  });
}

void castFromString(
    const SimpleVector<StringView>& inputVector,
    exec::EvalCtx& context,
    const SelectivityVector& rows,
    int64_t* rawResults) {
  context.applyToSelectedNoThrow(rows, [&](auto row) {
    const auto castResult = util::fromTimestampWithTimezoneString(
        inputVector.valueAt(row).data(),
        inputVector.valueAt(row).size(),
        util::TimestampParseMode::kPrestoCast);
    if (castResult.hasError()) {
      context.setStatus(row, castResult.error());
    } else {
      auto [ts, timeZone, millisOffset] = castResult.value();
      // Input string may not contain a timezone - if so, it is interpreted in
      // session timezone.
      if (timeZone == nullptr) {
        if (millisOffset.has_value()) {
          context.setStatus(
              row,
              Status::UserError(
                  "Unknown timezone value in: \"{}\"",
                  inputVector.valueAt(row)));
          return;
        }
        const auto& config = context.execCtx()->queryCtx()->queryConfig();
        timeZone = getTimeZoneFromConfig(config);
      }
      ts.toGMT(*timeZone);
      rawResults[row] = pack(ts.toMillis(), timeZone->id());
    }
  });
}

void castToString(
    const BaseVector& input,
    exec::EvalCtx& context,
    const SelectivityVector& rows,
    BaseVector& result) {
  auto* flatResult = result.as<FlatVector<StringView>>();
  const auto* timestamps = input.as<SimpleVector<int64_t>>();

  auto expectedFormatter =
      functions::buildJodaDateTimeFormatter("yyyy-MM-dd HH:mm:ss.SSS ZZZ");
  VELOX_CHECK(
      !expectedFormatter.hasError(),
      "Default format should always be valid, error: {}",
      expectedFormatter.error().message());
  auto formatter = expectedFormatter.value();
  context.applyToSelectedNoThrow(rows, [&](auto row) {
    const auto timestampWithTimezone = timestamps->valueAt(row);

    const auto timestamp = unpackTimestampUtc(timestampWithTimezone);
    const auto timeZoneId = unpackZoneKeyId(timestampWithTimezone);
    const auto* timezonePtr = tz::locateZone(tz::getTimeZoneName(timeZoneId));

    exec::StringWriter result(flatResult, row);

    const auto maxResultSize = formatter->maxResultSize(timezonePtr);
    result.reserve(maxResultSize);
    const auto resultSize =
        formatter->format(timestamp, timezonePtr, maxResultSize, result.data());
    result.resize(resultSize);

    result.finalize();
  });
}

void castToTimestamp(
    const BaseVector& input,
    exec::EvalCtx& context,
    const SelectivityVector& rows,
    BaseVector& result) {
  const auto& config = context.execCtx()->queryCtx()->queryConfig();
  const auto adjustTimestampToTimezone = config.adjustTimestampToTimezone();
  auto* flatResult = result.as<FlatVector<Timestamp>>();
  const auto* timestamps = input.as<SimpleVector<int64_t>>();

  context.applyToSelectedNoThrow(rows, [&](auto row) {
    auto timestampWithTimezone = timestamps->valueAt(row);
    auto ts = unpackTimestampUtc(timestampWithTimezone);
    if (!adjustTimestampToTimezone) {
      // Convert UTC to the given time zone.
      ts.toTimezone(*tz::locateZone(unpackZoneKeyId(timestampWithTimezone)));
    }
    flatResult->set(row, ts);
  });
}

void castToDate(
    const BaseVector& input,
    exec::EvalCtx& context,
    const SelectivityVector& rows,
    BaseVector& result) {
  auto* flatResult = result.as<FlatVector<int32_t>>();
  const auto* timestampVector = input.as<SimpleVector<int64_t>>();

  context.applyToSelectedNoThrow(rows, [&](auto row) {
    auto timestampWithTimezone = timestampVector->valueAt(row);
    auto timestamp = unpackTimestampUtc(timestampWithTimezone);
    timestamp.toTimezone(
        *tz::locateZone(unpackZoneKeyId(timestampWithTimezone)));

    const auto days = util::toDate(timestamp, nullptr);
    flatResult->set(row, days);
  });
}

class TimestampWithTimeZoneCastOperator : public exec::CastOperator {
 public:
  static const std::shared_ptr<const CastOperator>& get() {
    static const std::shared_ptr<const CastOperator> instance{
        new TimestampWithTimeZoneCastOperator()};

    return instance;
  }

  bool isSupportedFromType(const TypePtr& other) const override {
    switch (other->kind()) {
      case TypeKind::TIMESTAMP:
        return true;
      case TypeKind::VARCHAR:
        return true;
      case TypeKind::INTEGER:
        return other->isDate();
      default:
        return false;
    }
  }

  bool isSupportedToType(const TypePtr& other) const override {
    switch (other->kind()) {
      case TypeKind::TIMESTAMP:
        return true;
      case TypeKind::VARCHAR:
        return true;
      case TypeKind::INTEGER:
        return other->isDate();
      default:
        return false;
    }
  }

  void castTo(
      const BaseVector& input,
      exec::EvalCtx& context,
      const SelectivityVector& rows,
      const TypePtr& resultType,
      VectorPtr& result) const override {
    context.ensureWritable(rows, resultType, result);

    auto* timestampWithTzResult = result->asFlatVector<int64_t>();
    timestampWithTzResult->clearNulls(rows);

    auto* rawResults = timestampWithTzResult->mutableRawValues();

    if (input.typeKind() == TypeKind::TIMESTAMP) {
      const auto inputVector = input.as<SimpleVector<Timestamp>>();
      castFromTimestamp(*inputVector, context, rows, rawResults);
    } else if (input.typeKind() == TypeKind::VARCHAR) {
      const auto inputVector = input.as<SimpleVector<StringView>>();
      castFromString(*inputVector, context, rows, rawResults);
    } else if (input.typeKind() == TypeKind::INTEGER) {
      VELOX_CHECK(input.type()->isDate());
      const auto inputVector = input.as<SimpleVector<int32_t>>();
      castFromDate(*inputVector, context, rows, rawResults);
    } else {
      VELOX_UNSUPPORTED(
          "Cast from {} to TIMESTAMP WITH TIME ZONE not yet supported",
          input.type()->toString());
    }
  }

  void castFrom(
      const BaseVector& input,
      exec::EvalCtx& context,
      const SelectivityVector& rows,
      const TypePtr& resultType,
      VectorPtr& result) const override {
    context.ensureWritable(rows, resultType, result);

    if (resultType->kind() == TypeKind::TIMESTAMP) {
      castToTimestamp(input, context, rows, *result);
    } else if (resultType->kind() == TypeKind::VARCHAR) {
      castToString(input, context, rows, *result);
    } else if (resultType->kind() == TypeKind::INTEGER) {
      VELOX_CHECK(resultType->isDate());
      castToDate(input, context, rows, *result);
    } else {
      VELOX_UNSUPPORTED(
          "Cast from TIMESTAMP WITH TIME ZONE to {} not yet supported",
          resultType->toString());
    }
  }

 private:
  TimestampWithTimeZoneCastOperator() = default;
};

class TimestampWithTimeZoneTypeFactory : public CustomTypeFactory {
 public:
  TypePtr getType(const std::vector<TypeParameter>& parameters) const override {
    VELOX_CHECK(parameters.empty());
    return TIMESTAMP_WITH_TIME_ZONE();
  }

  // Type casting from and to TimestampWithTimezone is not supported yet.
  exec::CastOperatorPtr getCastOperator() const override {
    return TimestampWithTimeZoneCastOperator::get();
  }

  AbstractInputGeneratorPtr getInputGenerator(
      const InputGeneratorConfig& config) const override {
    return std::make_shared<fuzzer::TimestampWithTimeZoneInputGenerator>(
        config.seed_, config.nullRatio_);
  }
};
} // namespace

void registerTimestampWithTimeZoneType() {
  registerCustomType(
      "timestamp with time zone",
      std::make_unique<const TimestampWithTimeZoneTypeFactory>());
}
} // namespace facebook::velox
