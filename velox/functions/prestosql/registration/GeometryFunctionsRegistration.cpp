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

#include <string>
#include "velox/functions/Registerer.h"
#include "velox/functions/prestosql/GeometryFunctions.h"
#include "velox/functions/prestosql/types/GeometryRegistration.h"

namespace facebook::velox::functions {

namespace {

void registerConstructors(const std::string& prefix) {
  registerFunction<StGeometryFromTextFunction, Geometry, Varchar>(
      {{prefix + "ST_GeometryFromText"}});
  registerFunction<StGeomFromBinaryFunction, Geometry, Varbinary>(
      {{prefix + "ST_GeomFromBinary"}});
  registerFunction<StAsTextFunction, Varchar, Geometry>(
      {{prefix + "ST_AsText"}});
  registerFunction<StAsBinaryFunction, Varbinary, Geometry>(
      {{prefix + "ST_AsBinary"}});
  registerFunction<StPointFunction, Geometry, double, double>(
      {{prefix + "ST_Point"}});
}

void registerRelationPredicates(const std::string& prefix) {
  registerFunction<StRelateFunction, bool, Geometry, Geometry, Varchar>(
      {{prefix + "ST_Relate"}});

  registerFunction<StContainsFunction, bool, Geometry, Geometry>(
      {{prefix + "ST_Contains"}});
  registerFunction<StCrossesFunction, bool, Geometry, Geometry>(
      {{prefix + "ST_Crosses"}});
  registerFunction<StDisjointFunction, bool, Geometry, Geometry>(
      {{prefix + "ST_Disjoint"}});
  registerFunction<StEqualsFunction, bool, Geometry, Geometry>(
      {{prefix + "ST_Equals"}});
  registerFunction<StIntersectsFunction, bool, Geometry, Geometry>(
      {{prefix + "ST_Intersects"}});
  registerFunction<StOverlapsFunction, bool, Geometry, Geometry>(
      {{prefix + "ST_Overlaps"}});
  registerFunction<StTouchesFunction, bool, Geometry, Geometry>(
      {{prefix + "ST_Touches"}});
  registerFunction<StWithinFunction, bool, Geometry, Geometry>(
      {{prefix + "ST_Within"}});
}

void registerOverlayOperations(const std::string& prefix) {
  registerFunction<StBoundaryFunction, Geometry, Geometry>(
      {{prefix + "St_Boundary"}});
  registerFunction<StDifferenceFunction, Geometry, Geometry, Geometry>(
      {{prefix + "ST_Difference"}});
  registerFunction<StIntersectionFunction, Geometry, Geometry, Geometry>(
      {{prefix + "ST_Intersection"}});
  registerFunction<StSymDifferenceFunction, Geometry, Geometry, Geometry>(
      {{prefix + "ST_SymDifference"}});
  registerFunction<StUnionFunction, Geometry, Geometry, Geometry>(
      {{prefix + "ST_Union"}});
  registerFunction<StEnvelopeAsPtsFunction, Array<Geometry>, Geometry>(
      {{prefix + "ST_EnvelopeAsPts"}});
}

void registerAccessors(const std::string& prefix) {
  registerFunction<StIsValidFunction, bool, Geometry>(
      {{prefix + "ST_IsValid"}});
  registerFunction<StIsSimpleFunction, bool, Geometry>(
      {{prefix + "ST_IsSimple"}});
  registerFunction<GeometryInvalidReasonFunction, Varchar, Geometry>(
      {{prefix + "geometry_invalid_reason"}});
  registerFunction<SimplifyGeometryFunction, Geometry, Geometry, double>(
      {{prefix + "simplify_geometry"}});

  registerFunction<StAreaFunction, double, Geometry>({{prefix + "ST_Area"}});
  registerFunction<StCentroidFunction, Geometry, Geometry>(
      {{prefix + "ST_Centroid"}});
  registerFunction<StXFunction, double, Geometry>({{prefix + "ST_X"}});
  registerFunction<StYFunction, double, Geometry>({{prefix + "ST_Y"}});
  registerFunction<StXMinFunction, double, Geometry>({{prefix + "ST_XMin"}});
  registerFunction<StYMinFunction, double, Geometry>({{prefix + "ST_YMin"}});
  registerFunction<StXMaxFunction, double, Geometry>({{prefix + "ST_XMax"}});
  registerFunction<StYMaxFunction, double, Geometry>({{prefix + "ST_YMax"}});
  registerFunction<StGeometryTypeFunction, Varchar, Geometry>(
      {{prefix + "ST_GeometryType"}});
  registerFunction<StDistanceFunction, double, Geometry, Geometry>(
      {{prefix + "ST_Distance"}});
  registerFunction<StPolygonFunction, Geometry, Varchar>(
      {{prefix + "ST_Polygon"}});
  registerFunction<StIsClosedFunction, bool, Geometry>(
      {{prefix + "ST_IsClosed"}});
  registerFunction<StIsEmptyFunction, bool, Geometry>(
      {{prefix + "ST_IsEmpty"}});
  registerFunction<StIsRingFunction, bool, Geometry>({{prefix + "ST_IsRing"}});
  registerFunction<StLengthFunction, double, Geometry>(
      {{prefix + "ST_Length"}});
  registerFunction<StPointNFunction, Geometry, Geometry, int32_t>(
      {{prefix + "ST_PointN"}});
  registerFunction<StStartPointFunction, Geometry, Geometry>(
      {{prefix + "ST_StartPoint"}});
  registerFunction<StEndPointFunction, Geometry, Geometry>(
      {{prefix + "ST_EndPoint"}});
  registerFunction<StGeometryNFunction, Geometry, Geometry, int32_t>(
      {{prefix + "ST_GeometryN"}});
  registerFunction<StInteriorRingNFunction, Geometry, Geometry, int32_t>(
      {{prefix + "ST_InteriorRingN"}});
  registerFunction<StNumGeometriesFunction, int32_t, Geometry>(
      {{prefix + "ST_NumGeometries"}});
  registerFunction<StNumInteriorRingFunction, int32_t, Geometry>(
      {{prefix + "ST_NumInteriorRing"}});
  registerFunction<StConvexHullFunction, Geometry, Geometry>(
      {{prefix + "ST_ConvexHull"}});
  registerFunction<StDimensionFunction, int8_t, Geometry>(
      {{prefix + "ST_Dimension"}});
  registerFunction<StExteriorRingFunction, Geometry, Geometry>(
      {{prefix + "ST_ExteriorRing"}});
  registerFunction<StEnvelopeFunction, Geometry, Geometry>(
      {{prefix + "ST_Envelope"}});
  registerFunction<StBufferFunction, Geometry, Geometry, double>(
      {{prefix + "ST_Buffer"}});

  velox::exec::registerVectorFunction(
      prefix + "ST_CoordDim",
      StCoordDimFunction::signatures(),
      std::make_unique<StCoordDimFunction>());
  registerFunction<StPointsFunction, Array<Geometry>, Geometry>(
      {{prefix + "ST_Points"}});
  registerFunction<StNumPointsFunction, int32_t, Geometry>(
      {{prefix + "ST_NumPoints"}});
  registerFunction<
      GeometryNearestPointsFunction,
      Array<Geometry>,
      Geometry,
      Geometry>({{prefix + "geometry_nearest_points"}});
}

} // namespace

void registerGeometryFunctions(const std::string& prefix) {
  registerGeometryType();
  registerConstructors(prefix);
  registerRelationPredicates(prefix);
  registerOverlayOperations(prefix);
  registerAccessors(prefix);
}

} // namespace facebook::velox::functions
