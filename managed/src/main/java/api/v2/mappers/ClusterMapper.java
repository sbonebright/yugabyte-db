// Copyright (c) YugaByte, Inc.
package api.v2.mappers;

import api.v2.models.ClusterInfo;
import api.v2.models.ClusterSpec;
import api.v2.models.PlacementAZ;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import org.mapstruct.InheritInverseConfiguration;
import org.mapstruct.Mapper;
import org.mapstruct.Mapping;
import org.mapstruct.NullValueCheckStrategy;

@Mapper(
    config = CentralConfig.class,
    uses = {UserIntentMapper.class})
public interface ClusterMapper {
  @Mapping(target = ".", source = "userIntent")
  @Mapping(target = "storageSpec", source = "userIntent")
  @Mapping(target = "networkingSpec", source = "userIntent")
  @Mapping(target = "providerSpec", source = "userIntent")
  @Mapping(target = "placementSpec", source = "placementInfo")
  @Mapping(target = "providerSpec.imageBundleUuid", source = "userIntent.imageBundleUUID")
  @Mapping(
      target = "providerSpec.provider",
      expression = "java(UUID.fromString(userIntent.provider))")
  @Mapping(target = "useSpotInstance", source = "userIntent.useSpotInstance")
  @Mapping(target = "gflags", source = "userIntent")
  ClusterSpec toV2ClusterSpec(Cluster v1Cluster);

  @Mapping(target = "userIntent", source = ".")
  @Mapping(target = "placementInfo", source = "placementSpec")
  // null check is required here to avoid overwriting the auto generated uuid with null
  @Mapping(target = "uuid", nullValueCheckStrategy = NullValueCheckStrategy.ALWAYS)
  Cluster toV1Cluster(ClusterSpec clusterSpec);

  @Mapping(target = ".", source = "userIntent")
  ClusterInfo toV2ClusterInfo(Cluster v1Cluster);

  // used implicitly in above mapping
  @Mapping(target = "numNodesInAZ", source = "numNodesInAz")
  @Mapping(target = "isAffinitized", source = "leaderAffinity")
  PlacementInfo.PlacementAZ toV1PlacementAZ(PlacementAZ placementAZ);

  @InheritInverseConfiguration
  PlacementAZ toV2PlacementAZ(PlacementInfo.PlacementAZ placementAZ);
}
