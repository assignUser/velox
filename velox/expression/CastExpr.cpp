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

#include "velox/expression/CastExpr.h"

#include <fmt/format.h>
#include <stdexcept>

#include "velox/common/base/Exceptions.h"
#include "velox/core/CoreTypeSystem.h"
#include "velox/expression/PeeledEncoding.h"
#include "velox/expression/PrestoCastHooks.h"
#include "velox/expression/ScopedVarSetter.h"
#include "velox/functions/lib/RowsTranslationUtil.h"
#include "velox/type/Type.h"
#include "velox/type/tz/TimeZoneMap.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FunctionVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/SelectivityVector.h"

namespace facebook::velox::exec {

namespace {

const tz::TimeZone* getTimeZoneFromConfig(const core::QueryConfig& config) {
  if (config.adjustTimestampToTimezone()) {
    const auto sessionTzName = config.sessionTimezone();
    if (!sessionTzName.empty()) {
      return tz::locateZone(sessionTzName);
    }
  }
  return nullptr;
}

} // namespace

VectorPtr CastExpr::castFromDate(
    const SelectivityVector& rows,
    const BaseVector& input,
    exec::EvalCtx& context,
    const TypePtr& toType) {
  VectorPtr castResult;
  context.ensureWritable(rows, toType, castResult);
  (*castResult).clearNulls(rows);

  auto* inputFlatVector = input.as<SimpleVector<int32_t>>();
  switch (toType->kind()) {
    case TypeKind::VARCHAR: {
      auto* resultFlatVector = castResult->as<FlatVector<StringView>>();
      applyToSelectedNoThrowLocal(context, rows, castResult, [&](int row) {
        try {
          // TODO Optimize to avoid creating an intermediate string.
          auto output = DATE()->toString(inputFlatVector->valueAt(row));
          auto writer = exec::StringWriter(resultFlatVector, row);
          writer.resize(output.size());
          ::memcpy(writer.data(), output.data(), output.size());
          writer.finalize();
        } catch (const VeloxException& ue) {
          if (!ue.isUserError()) {
            throw;
          }
          VELOX_USER_FAIL(
              makeErrorMessage(input, row, toType) + " " + ue.message());
        } catch (const std::exception& e) {
          VELOX_USER_FAIL(
              makeErrorMessage(input, row, toType) + " " + e.what());
        }
      });
      return castResult;
    }
    case TypeKind::TIMESTAMP: {
      static const int64_t kMillisPerDay{86'400'000};
      const auto* timeZone =
          getTimeZoneFromConfig(context.execCtx()->queryCtx()->queryConfig());
      auto* resultFlatVector = castResult->as<FlatVector<Timestamp>>();
      applyToSelectedNoThrowLocal(context, rows, castResult, [&](int row) {
        auto timestamp = Timestamp::fromMillis(
            inputFlatVector->valueAt(row) * kMillisPerDay);
        if (timeZone) {
          timestamp.toGMT(*timeZone);
        }
        resultFlatVector->set(row, timestamp);
      });

      return castResult;
    }
    default:
      VELOX_UNSUPPORTED(
          "Cast from DATE to {} is not supported", toType->toString());
  }
}

VectorPtr CastExpr::castToDate(
    const SelectivityVector& rows,
    const BaseVector& input,
    exec::EvalCtx& context,
    const TypePtr& fromType) {
  VectorPtr castResult;
  context.ensureWritable(rows, DATE(), castResult);
  (*castResult).clearNulls(rows);
  auto* resultFlatVector = castResult->as<FlatVector<int32_t>>();
  switch (fromType->kind()) {
    case TypeKind::VARCHAR: {
      auto* inputVector = input.as<SimpleVector<StringView>>();
      applyToSelectedNoThrowLocal(context, rows, castResult, [&](int row) {
        bool wrapException = true;
        try {
          const auto result =
              hooks_->castStringToDate(inputVector->valueAt(row));
          if (result.hasError()) {
            wrapException = false;
            if (setNullInResultAtError()) {
              resultFlatVector->setNull(row, true);
            } else {
              if (context.captureErrorDetails()) {
                context.setStatus(
                    row,
                    Status::UserError(
                        "{} {}",
                        makeErrorMessage(input, row, DATE()),
                        result.error().message()));
              } else {
                context.setStatus(row, Status::UserError());
              }
            }
          } else {
            resultFlatVector->set(row, result.value());
          }
        } catch (const VeloxUserError& ue) {
          if (!wrapException) {
            throw;
          }
          VELOX_USER_FAIL(
              makeErrorMessage(input, row, DATE()) + " " + ue.message());
        } catch (const std::exception& e) {
          VELOX_USER_FAIL(
              makeErrorMessage(input, row, DATE()) + " " + e.what());
        }
      });

      return castResult;
    }
    case TypeKind::TIMESTAMP: {
      auto* inputVector = input.as<SimpleVector<Timestamp>>();
      const auto* timeZone =
          getTimeZoneFromConfig(context.execCtx()->queryCtx()->queryConfig());
      applyToSelectedNoThrowLocal(context, rows, castResult, [&](int row) {
        const auto days = util::toDate(inputVector->valueAt(row), timeZone);
        resultFlatVector->set(row, days);
      });
      return castResult;
    }
    default:
      VELOX_UNSUPPORTED(
          "Cast from {} to DATE is not supported", fromType->toString());
  }
}

VectorPtr CastExpr::castFromIntervalDayTime(
    const SelectivityVector& rows,
    const BaseVector& input,
    exec::EvalCtx& context,
    const TypePtr& toType) {
  VectorPtr castResult;
  context.ensureWritable(rows, toType, castResult);
  (*castResult).clearNulls(rows);

  auto* inputFlatVector = input.as<SimpleVector<int64_t>>();
  switch (toType->kind()) {
    case TypeKind::VARCHAR: {
      auto* resultFlatVector = castResult->as<FlatVector<StringView>>();
      applyToSelectedNoThrowLocal(context, rows, castResult, [&](int row) {
        try {
          // TODO Optimize to avoid creating an intermediate string.
          auto output =
              INTERVAL_DAY_TIME()->valueToString(inputFlatVector->valueAt(row));
          auto writer = exec::StringWriter(resultFlatVector, row);
          writer.resize(output.size());
          ::memcpy(writer.data(), output.data(), output.size());
          writer.finalize();
        } catch (const VeloxException& ue) {
          if (!ue.isUserError()) {
            throw;
          }
          VELOX_USER_FAIL(
              makeErrorMessage(input, row, toType) + " " + ue.message());
        } catch (const std::exception& e) {
          VELOX_USER_FAIL(
              makeErrorMessage(input, row, toType) + " " + e.what());
        }
      });
      return castResult;
    }
    default:
      VELOX_UNSUPPORTED(
          "Cast from {} to {} is not supported",
          INTERVAL_DAY_TIME()->toString(),
          toType->toString());
  }
}

namespace {
void propagateErrorsOrSetNulls(
    bool setNullInResultAtError,
    EvalCtx& context,
    const SelectivityVector& nestedRows,
    const BufferPtr& elementToTopLevelRows,
    VectorPtr& result,
    EvalErrorsPtr& oldErrors) {
  if (context.errors()) {
    if (setNullInResultAtError) {
      // Errors in context.errors() should be translated to nulls in the top
      // level rows.
      context.convertElementErrorsToTopLevelNulls(
          nestedRows, elementToTopLevelRows, result);
    } else {
      context.addElementErrorsToTopLevel(
          nestedRows, elementToTopLevelRows, oldErrors);
    }
  }
}
} // namespace

#define VELOX_DYNAMIC_DECIMAL_TYPE_DISPATCH(       \
    TEMPLATE_FUNC, decimalTypePtr, ...)            \
  [&]() {                                          \
    if (decimalTypePtr->isLongDecimal()) {         \
      return TEMPLATE_FUNC<int128_t>(__VA_ARGS__); \
    } else {                                       \
      return TEMPLATE_FUNC<int64_t>(__VA_ARGS__);  \
    }                                              \
  }()

VectorPtr CastExpr::applyMap(
    const SelectivityVector& rows,
    const MapVector* input,
    exec::EvalCtx& context,
    const MapType& fromType,
    const MapType& toType) {
  // Cast input keys/values vector to output keys/values vector using their
  // element selectivity vector

  // Initialize nested rows
  auto mapKeys = input->mapKeys();
  auto mapValues = input->mapValues();

  SelectivityVector nestedRows;
  BufferPtr elementToTopLevelRows;
  if (fromType.keyType() != toType.keyType() ||
      fromType.valueType() != toType.valueType()) {
    nestedRows = functions::toElementRows(mapKeys->size(), rows, input);
    elementToTopLevelRows = functions::getElementToTopLevelRows(
        mapKeys->size(), rows, input, context.pool());
  }

  EvalErrorsPtr oldErrors;
  context.swapErrors(oldErrors);

  // Cast keys
  VectorPtr newMapKeys;
  if (*fromType.keyType() == *toType.keyType()) {
    newMapKeys = input->mapKeys();
  } else {
    {
      ScopedVarSetter holder(&inTopLevel, false);
      apply(
          nestedRows,
          mapKeys,
          context,
          fromType.keyType(),
          toType.keyType(),
          newMapKeys);
    }
  }

  // Cast values
  VectorPtr newMapValues;
  if (*fromType.valueType() == *toType.valueType()) {
    newMapValues = mapValues;
  } else {
    {
      ScopedVarSetter holder(&inTopLevel, false);
      apply(
          nestedRows,
          mapValues,
          context,
          fromType.valueType(),
          toType.valueType(),
          newMapValues);
    }
  }

  // Returned map vector should be addressable for every element, even those
  // that are not selected.
  BufferPtr sizes = input->sizes();
  if (newMapKeys->isConstantEncoding() && newMapValues->isConstantEncoding()) {
    // We extends size since that is cheap.
    newMapKeys->resize(input->mapKeys()->size());
    newMapValues->resize(input->mapValues()->size());
  } else if (
      newMapKeys->size() < input->mapKeys()->size() ||
      newMapValues->size() < input->mapValues()->size()) {
    sizes =
        AlignedBuffer::allocate<vector_size_t>(rows.end(), context.pool(), 0);
    auto* inputSizes = input->rawSizes();
    auto* rawSizes = sizes->asMutable<vector_size_t>();

    rows.applyToSelected(
        [&](vector_size_t row) { rawSizes[row] = inputSizes[row]; });
  }

  // Assemble the output map
  VectorPtr result = std::make_shared<MapVector>(
      context.pool(),
      MAP(toType.keyType(), toType.valueType()),
      input->nulls(),
      rows.end(),
      input->offsets(),
      sizes,
      newMapKeys,
      newMapValues);

  propagateErrorsOrSetNulls(
      setNullInResultAtError(),
      context,
      nestedRows,
      elementToTopLevelRows,
      result,
      oldErrors);

  // Restore original state.
  context.swapErrors(oldErrors);
  return result;
}

VectorPtr CastExpr::applyArray(
    const SelectivityVector& rows,
    const ArrayVector* input,
    exec::EvalCtx& context,
    const ArrayType& fromType,
    const ArrayType& toType) {
  // Cast input array elements to output array elements based on their types
  // using their linear selectivity vector
  auto arrayElements = input->elements();

  auto nestedRows =
      functions::toElementRows(arrayElements->size(), rows, input);
  auto elementToTopLevelRows = functions::getElementToTopLevelRows(
      arrayElements->size(), rows, input, context.pool());

  EvalErrorsPtr oldErrors;
  context.swapErrors(oldErrors);

  VectorPtr newElements;
  {
    ScopedVarSetter holder(&inTopLevel, false);
    apply(
        nestedRows,
        arrayElements,
        context,
        fromType.elementType(),
        toType.elementType(),
        newElements);
  }

  // Returned array vector should be addressable for every element, even those
  // that are not selected.
  BufferPtr sizes = input->sizes();
  if (newElements->isConstantEncoding()) {
    // If the newElements we extends its size since that is cheap.
    newElements->resize(input->elements()->size());
  } else if (newElements->size() < input->elements()->size()) {
    sizes =
        AlignedBuffer::allocate<vector_size_t>(rows.end(), context.pool(), 0);
    auto* inputSizes = input->rawSizes();
    auto* rawSizes = sizes->asMutable<vector_size_t>();
    rows.applyToSelected(
        [&](vector_size_t row) { rawSizes[row] = inputSizes[row]; });
  }

  VectorPtr result = std::make_shared<ArrayVector>(
      context.pool(),
      ARRAY(toType.elementType()),
      input->nulls(),
      rows.end(),
      input->offsets(),
      sizes,
      newElements);

  propagateErrorsOrSetNulls(
      setNullInResultAtError(),
      context,
      nestedRows,
      elementToTopLevelRows,
      result,
      oldErrors);
  // Restore original state.
  context.swapErrors(oldErrors);
  return result;
}

VectorPtr CastExpr::applyRow(
    const SelectivityVector& rows,
    const RowVector* input,
    exec::EvalCtx& context,
    const RowType& fromType,
    const TypePtr& toType) {
  const RowType& toRowType = toType->asRow();
  int numInputChildren = input->children().size();
  int numOutputChildren = toRowType.size();

  // Extract the flag indicating matching of children must be done by name or
  // position
  auto matchByName =
      context.execCtx()->queryCtx()->queryConfig().isMatchStructByName();

  // Cast each row child to its corresponding output child
  std::vector<VectorPtr> newChildren;
  newChildren.reserve(numOutputChildren);

  EvalErrorsPtr oldErrors;
  if (setNullInResultAtError()) {
    // We need to isolate errors that happen during the cast from previous
    // errors since those translate to nulls, unlike exisiting errors.
    context.swapErrors(oldErrors);
  }

  for (auto toChildrenIndex = 0; toChildrenIndex < numOutputChildren;
       toChildrenIndex++) {
    // For each child, find the corresponding column index in the output
    const auto& toFieldName = toRowType.nameOf(toChildrenIndex);
    bool matchNotFound = false;

    // If match is by field name and the input field name is not found
    // in the output row type, do not consider it in the output
    int fromChildrenIndex = -1;
    if (matchByName) {
      if (!fromType.containsChild(toFieldName)) {
        matchNotFound = true;
      } else {
        fromChildrenIndex = fromType.getChildIdx(toFieldName);
        toChildrenIndex = toRowType.getChildIdx(toFieldName);
      }
    } else {
      fromChildrenIndex = toChildrenIndex;
      if (fromChildrenIndex >= numInputChildren) {
        matchNotFound = true;
      }
    }

    // Updating output types and names
    VectorPtr outputChild;
    const auto& toChildType = toRowType.childAt(toChildrenIndex);

    if (matchNotFound) {
      // Create a vector for null for this child
      context.ensureWritable(rows, toChildType, outputChild);
      outputChild->addNulls(rows);
    } else {
      const auto& inputChild = input->children()[fromChildrenIndex];
      if (*toChildType == *inputChild->type()) {
        outputChild = inputChild;
      } else {
        // Apply cast for the child.
        ScopedVarSetter holder(&inTopLevel, false);
        apply(
            rows,
            inputChild,
            context,
            inputChild->type(),
            toChildType,
            outputChild);
      }
    }
    newChildren.emplace_back(std::move(outputChild));
  }

  // Assemble the output row
  VectorPtr result = std::make_shared<RowVector>(
      context.pool(),
      toType,
      input->nulls(),
      rows.end(),
      std::move(newChildren));

  if (setNullInResultAtError()) {
    // Set errors as nulls.
    if (auto errors = context.errors()) {
      rows.applyToSelected([&](auto row) {
        if (errors->hasErrorAt(row)) {
          result->setNull(row, true);
        }
      });
    }
    // Restore original state.
    context.swapErrors(oldErrors);
  }

  return result;
}

template <typename toDecimalType>
VectorPtr CastExpr::applyDecimal(
    const SelectivityVector& rows,
    const BaseVector& input,
    exec::EvalCtx& context,
    const TypePtr& fromType,
    const TypePtr& toType) {
  VectorPtr castResult;
  context.ensureWritable(rows, toType, castResult);
  (*castResult).clearNulls(rows);

  // toType is a decimal
  switch (fromType->kind()) {
    case TypeKind::BOOLEAN:
      applyIntToDecimalCastKernel<bool, toDecimalType>(
          rows, input, context, toType, castResult);
      break;
    case TypeKind::TINYINT:
      applyIntToDecimalCastKernel<int8_t, toDecimalType>(
          rows, input, context, toType, castResult);
      break;
    case TypeKind::SMALLINT:
      applyIntToDecimalCastKernel<int16_t, toDecimalType>(
          rows, input, context, toType, castResult);
      break;
    case TypeKind::INTEGER:
      applyIntToDecimalCastKernel<int32_t, toDecimalType>(
          rows, input, context, toType, castResult);
      break;
    case TypeKind::REAL:
      applyFloatingPointToDecimalCastKernel<float, toDecimalType>(
          rows, input, context, toType, castResult);
      break;
    case TypeKind::DOUBLE:
      applyFloatingPointToDecimalCastKernel<double, toDecimalType>(
          rows, input, context, toType, castResult);
      break;
    case TypeKind::BIGINT: {
      if (fromType->isShortDecimal()) {
        applyDecimalCastKernel<int64_t, toDecimalType>(
            rows, input, context, fromType, toType, castResult);
        break;
      }
      applyIntToDecimalCastKernel<int64_t, toDecimalType>(
          rows, input, context, toType, castResult);
      break;
    }
    case TypeKind::HUGEINT: {
      if (fromType->isLongDecimal()) {
        applyDecimalCastKernel<int128_t, toDecimalType>(
            rows, input, context, fromType, toType, castResult);
        break;
      }
      [[fallthrough]];
    }
    case TypeKind::VARCHAR:
      applyVarcharToDecimalCastKernel<toDecimalType>(
          rows, input, context, toType, castResult);
      break;
    default:
      VELOX_UNSUPPORTED(
          "Cast from {} to {} is not supported",
          fromType->toString(),
          toType->toString());
  }
  return castResult;
}

void CastExpr::applyPeeled(
    const SelectivityVector& rows,
    const BaseVector& input,
    exec::EvalCtx& context,
    const TypePtr& fromType,
    const TypePtr& toType,
    VectorPtr& result) {
  auto castFromOperator = getCastOperator(fromType);
  if (castFromOperator && !castFromOperator->isSupportedToType(toType)) {
    VELOX_USER_FAIL(
        "Cannot cast {} to {}.", fromType->toString(), toType->toString());
  }

  auto castToOperator = getCastOperator(toType);
  if (castToOperator && !castToOperator->isSupportedFromType(fromType)) {
    VELOX_USER_FAIL(
        "Cannot cast {} to {}.", fromType->toString(), toType->toString());
  }

  if (castFromOperator || castToOperator) {
    VELOX_USER_CHECK(
        *fromType != *toType,
        "Attempting to cast from {} to itself.",
        fromType->toString());

    auto applyCustomCast = [&]() {
      if (castToOperator) {
        castToOperator->castTo(input, context, rows, toType, result, hooks_);
      } else {
        castFromOperator->castFrom(input, context, rows, toType, result);
      }
    };

    if (setNullInResultAtError()) {
      // This can be optimized by passing setNullInResultAtError() to castTo and
      // castFrom operations.

      EvalErrorsPtr oldErrors;
      context.swapErrors(oldErrors);

      applyCustomCast();

      if (context.errors()) {
        auto errors = context.errors();
        auto rawNulls = result->mutableRawNulls();

        rows.applyToSelected([&](auto row) {
          if (errors->hasErrorAt(row)) {
            bits::setNull(rawNulls, row, true);
          }
        });
      };
      // Restore original state.
      context.swapErrors(oldErrors);

    } else {
      applyCustomCast();
    }
  } else if (fromType->isDate()) {
    result = castFromDate(rows, input, context, toType);
  } else if (toType->isDate()) {
    result = castToDate(rows, input, context, fromType);
  } else if (fromType->isIntervalDayTime()) {
    result = castFromIntervalDayTime(rows, input, context, toType);
  } else if (toType->isIntervalDayTime()) {
    VELOX_UNSUPPORTED(
        "Cast from {} to {} is not supported",
        fromType->toString(),
        toType->toString());
  } else if (toType->isShortDecimal()) {
    result = applyDecimal<int64_t>(rows, input, context, fromType, toType);
  } else if (toType->isLongDecimal()) {
    result = applyDecimal<int128_t>(rows, input, context, fromType, toType);
  } else if (fromType->isDecimal()) {
    switch (toType->kind()) {
      case TypeKind::VARCHAR:
        result = VELOX_DYNAMIC_DECIMAL_TYPE_DISPATCH(
            applyDecimalToVarcharCast,
            fromType,
            rows,
            input,
            context,
            fromType);
        break;
      default:
        result = VELOX_DYNAMIC_DECIMAL_TYPE_DISPATCH(
            applyDecimalToPrimitiveCast,
            fromType,
            rows,
            input,
            context,
            fromType,
            toType);
    }
  } else if (
      fromType->kind() == TypeKind::TIMESTAMP &&
      (toType->kind() == TypeKind::VARCHAR ||
       toType->kind() == TypeKind::VARBINARY)) {
    result = applyTimestampToVarcharCast(toType, rows, context, input);
  } else if (toType->kind() == TypeKind::VARBINARY) {
    switch (fromType->kind()) {
      case TypeKind::TINYINT:
        result = applyIntToBinaryCast<int8_t>(rows, context, input);
        break;
      case TypeKind::SMALLINT:
        result = applyIntToBinaryCast<int16_t>(rows, context, input);
        break;
      case TypeKind::INTEGER:
        result = applyIntToBinaryCast<int32_t>(rows, context, input);
        break;
      case TypeKind::BIGINT:
        result = applyIntToBinaryCast<int64_t>(rows, context, input);
        break;
      default:
        // Handle primitive type conversions.
        applyCastPrimitivesDispatch<TypeKind::VARBINARY>(
            fromType, toType, rows, context, input, result);
        break;
    }
  } else {
    switch (toType->kind()) {
      case TypeKind::MAP:
        result = applyMap(
            rows,
            input.asUnchecked<MapVector>(),
            context,
            fromType->asMap(),
            toType->asMap());
        break;
      case TypeKind::ARRAY:
        result = applyArray(
            rows,
            input.asUnchecked<ArrayVector>(),
            context,
            fromType->asArray(),
            toType->asArray());
        break;
      case TypeKind::ROW:
        result = applyRow(
            rows,
            input.asUnchecked<RowVector>(),
            context,
            fromType->asRow(),
            toType);
        break;
      default: {
        // Handle primitive type conversions.
        VELOX_DYNAMIC_SCALAR_TYPE_DISPATCH(
            applyCastPrimitivesDispatch,
            toType->kind(),
            fromType,
            toType,
            rows,
            context,
            input,
            result);
      }
    }
  }
}

VectorPtr CastExpr::applyTimestampToVarcharCast(
    const TypePtr& toType,
    const SelectivityVector& rows,
    exec::EvalCtx& context,
    const BaseVector& input) {
  VectorPtr result;
  context.ensureWritable(rows, toType, result);
  (*result).clearNulls(rows);
  auto flatResult = result->asFlatVector<StringView>();
  const auto simpleInput = input.as<SimpleVector<Timestamp>>();

  const auto& options = hooks_->timestampToStringOptions();
  const uint32_t rowSize = getMaxStringLength(options);

  Buffer* buffer = flatResult->getBufferWithSpace(
      rows.countSelected() * rowSize, true /*exactSize*/);
  char* rawBuffer = buffer->asMutable<char>() + buffer->size();

  applyToSelectedNoThrowLocal(context, rows, result, [&](vector_size_t row) {
    // Adjust input timestamp according the session timezone.
    Timestamp inputValue(simpleInput->valueAt(row));
    if (options.timeZone) {
      inputValue.toTimezone(*(options.timeZone));
    }
    const auto stringView =
        Timestamp::tsToStringView(inputValue, options, rawBuffer);
    flatResult->setNoCopy(row, stringView);
    // The result of both Presto and Spark contains more than 12 digits even
    // when 'zeroPaddingYear' is disabled.
    VELOX_DCHECK(!stringView.isInline());
    rawBuffer += stringView.size();
  });

  // Update the exact buffer size.
  buffer->setSize(rawBuffer - buffer->asMutable<char>());
  return result;
}

template <typename TInput>
VectorPtr CastExpr::applyIntToBinaryCast(
    const SelectivityVector& rows,
    exec::EvalCtx& context,
    const BaseVector& input) {
  auto result = BaseVector::create(VARBINARY(), rows.end(), context.pool());
  const auto flatResult = result->asFlatVector<StringView>();
  const auto simpleInput = input.as<SimpleVector<TInput>>();

  // The created string view is always inlined for int types.
  char inlined[sizeof(TInput)];
  applyToSelectedNoThrowLocal(context, rows, result, [&](vector_size_t row) {
    TInput input = simpleInput->valueAt(row);
    if constexpr (std::is_same_v<TInput, int8_t>) {
      inlined[0] = static_cast<char>(input & 0xFF);
    } else {
      for (int i = sizeof(TInput) - 1; i >= 0; --i) {
        inlined[i] = static_cast<char>(input & 0xFF);
        input >>= 8;
      }
    }
    const auto stringView = StringView(inlined, sizeof(TInput));
    flatResult->setNoCopy(row, stringView);
  });

  return result;
}

void CastExpr::apply(
    const SelectivityVector& rows,
    const VectorPtr& input,
    exec::EvalCtx& context,
    const TypePtr& fromType,
    const TypePtr& toType,
    VectorPtr& result) {
  LocalSelectivityVector remainingRows(context, rows);

  context.deselectErrors(*remainingRows);

  LocalDecodedVector decoded(context, *input, *remainingRows);
  auto* rawNulls = decoded->nulls(remainingRows.get());

  if (rawNulls) {
    remainingRows->deselectNulls(
        rawNulls, remainingRows->begin(), remainingRows->end());
  }

  VectorPtr localResult;
  if (!remainingRows->hasSelections()) {
    localResult =
        BaseVector::createNullConstant(toType, rows.end(), context.pool());
  } else if (decoded->isIdentityMapping()) {
    applyPeeled(
        *remainingRows,
        *decoded->base(),
        context,
        fromType,
        toType,
        localResult);
  } else {
    withContextSaver([&](ContextSaver& saver) {
      LocalSelectivityVector newRowsHolder(*context.execCtx());

      LocalDecodedVector localDecoded(context);
      std::vector<VectorPtr> peeledVectors;
      auto peeledEncoding = PeeledEncoding::peel(
          {input}, *remainingRows, localDecoded, true, peeledVectors);
      VELOX_CHECK_EQ(peeledVectors.size(), 1);
      if (peeledVectors[0]->isLazy()) {
        peeledVectors[0] =
            peeledVectors[0]->as<LazyVector>()->loadedVectorShared();
      }
      auto newRows =
          peeledEncoding->translateToInnerRows(*remainingRows, newRowsHolder);
      // Save context and set the peel.
      context.saveAndReset(saver, *remainingRows);
      context.setPeeledEncoding(peeledEncoding);
      applyPeeled(
          *newRows, *peeledVectors[0], context, fromType, toType, localResult);

      localResult = context.getPeeledEncoding()->wrap(
          toType, context.pool(), localResult, *remainingRows);
    });
  }
  context.moveOrCopyResult(localResult, *remainingRows, result);
  context.releaseVector(localResult);

  // If there are nulls or rows that encountered errors in the input, add nulls
  // to the result at the same rows.
  VELOX_CHECK_NOT_NULL(result);
  if (rawNulls || context.errors()) {
    EvalCtx::addNulls(
        rows, remainingRows->asRange().bits(), context, toType, result);
  }
}

void CastExpr::evalSpecialForm(
    const SelectivityVector& rows,
    EvalCtx& context,
    VectorPtr& result) {
  VectorPtr input;
  inputs_[0]->eval(rows, context, input);
  auto fromType = inputs_[0]->type();
  auto toType = std::const_pointer_cast<const Type>(type_);

  inTopLevel = true;
  if (isTryCast()) {
    ScopedVarSetter holder{context.mutableThrowOnError(), false};
    ScopedVarSetter captureErrorDetails(
        context.mutableCaptureErrorDetails(), false);

    ScopedThreadSkipErrorDetails skipErrorDetails(true);

    apply(rows, input, context, fromType, toType, result);
  } else {
    apply(rows, input, context, fromType, toType, result);
  }
  // Return 'input' back to the vector pool in 'context' so it can be reused.
  context.releaseVector(input);
}

std::string CastExpr::toString(bool recursive) const {
  std::stringstream out;
  out << name() << "(";
  if (recursive) {
    appendInputs(out);
  } else {
    out << inputs_[0]->toString(false);
  }
  out << " as " << type_->toString() << ")";
  return out.str();
}

std::string CastExpr::toSql(std::vector<VectorPtr>* complexConstants) const {
  std::stringstream out;
  out << name() << "(";
  appendInputsSql(out, complexConstants);
  out << " as ";
  toTypeSql(type_, out);
  out << ")";
  return out.str();
}

CastOperatorPtr CastExpr::getCastOperator(const TypePtr& type) {
  const auto* key = type->name();

  auto it = castOperators_.find(key);
  if (it != castOperators_.end()) {
    return it->second;
  }

  auto castOperator = getCustomTypeCastOperator(key);
  if (castOperator == nullptr) {
    return nullptr;
  }

  castOperators_.emplace(key, castOperator);
  return castOperator;
}

TypePtr CastCallToSpecialForm::resolveType(
    const std::vector<TypePtr>& /* argTypes */) {
  VELOX_FAIL("CAST expressions do not support type resolution.");
}

ExprPtr CastCallToSpecialForm::constructSpecialForm(
    const TypePtr& type,
    std::vector<ExprPtr>&& compiledChildren,
    bool trackCpuUsage,
    const core::QueryConfig& config) {
  VELOX_CHECK_EQ(
      compiledChildren.size(),
      1,
      "CAST statements expect exactly 1 argument, received {}.",
      compiledChildren.size());
  const auto inputKind = compiledChildren[0]->type()->kind();
  if (type->kind() == TypeKind::VARBINARY &&
      (inputKind == TypeKind::TINYINT || inputKind == TypeKind::SMALLINT ||
       inputKind == TypeKind::INTEGER || inputKind == TypeKind::BIGINT)) {
    VELOX_UNSUPPORTED(
        "Cannot cast {} to VARBINARY.",
        compiledChildren[0]->type()->toString());
  }
  return std::make_shared<CastExpr>(
      type,
      std::move(compiledChildren[0]),
      trackCpuUsage,
      false,
      std::make_shared<PrestoCastHooks>(config));
}

TypePtr TryCastCallToSpecialForm::resolveType(
    const std::vector<TypePtr>& /* argTypes */) {
  VELOX_FAIL("TRY CAST expressions do not support type resolution.");
}

ExprPtr TryCastCallToSpecialForm::constructSpecialForm(
    const TypePtr& type,
    std::vector<ExprPtr>&& compiledChildren,
    bool trackCpuUsage,
    const core::QueryConfig& config) {
  VELOX_CHECK_EQ(
      compiledChildren.size(),
      1,
      "TRY CAST statements expect exactly 1 argument, received {}.",
      compiledChildren.size());
  return std::make_shared<CastExpr>(
      type,
      std::move(compiledChildren[0]),
      trackCpuUsage,
      true,
      std::make_shared<PrestoCastHooks>(config));
}
} // namespace facebook::velox::exec
