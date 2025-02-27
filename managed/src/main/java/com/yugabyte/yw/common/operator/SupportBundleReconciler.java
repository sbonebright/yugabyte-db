package com.yugabyte.yw.common.operator;

import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.TaskExecutor;
import com.yugabyte.yw.commissioner.tasks.params.SupportBundleTaskParams;
import com.yugabyte.yw.common.SupportBundleUtil;
import com.yugabyte.yw.common.operator.utils.OperatorUtils;
import com.yugabyte.yw.forms.SupportBundleFormData;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.SupportBundle;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.BundleDetails.ComponentType;
import com.yugabyte.yw.models.helpers.TaskType;
import io.fabric8.kubernetes.api.model.KubernetesResourceList;
import io.fabric8.kubernetes.client.dsl.MixedOperation;
import io.fabric8.kubernetes.client.dsl.Resource;
import io.fabric8.kubernetes.client.informers.ResourceEventHandler;
import io.fabric8.kubernetes.client.informers.SharedIndexInformer;
import io.fabric8.kubernetes.client.informers.cache.Lister;
import io.yugabyte.operator.v1alpha1.SupportBundleStatus;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.time.Instant;
import java.util.Date;
import java.util.EnumSet;
import java.util.UUID;
import java.util.stream.Collectors;
import lombok.extern.slf4j.Slf4j;

@Slf4j
public class SupportBundleReconciler
    implements ResourceEventHandler<io.yugabyte.operator.v1alpha1.SupportBundle>, Runnable {
  private final SharedIndexInformer<io.yugabyte.operator.v1alpha1.SupportBundle> informer;
  private final Lister<io.yugabyte.operator.v1alpha1.SupportBundle> lister;
  private final MixedOperation<
          io.yugabyte.operator.v1alpha1.SupportBundle,
          KubernetesResourceList<io.yugabyte.operator.v1alpha1.SupportBundle>,
          Resource<io.yugabyte.operator.v1alpha1.SupportBundle>>
      resourceClient;
  private final String namespace;
  private final Commissioner commissioner;
  private final TaskExecutor taskExecutor;

  private final SupportBundleUtil supportBundleUtil;
  private final OperatorUtils operatorUtils;

  public SupportBundleReconciler(
      SharedIndexInformer<io.yugabyte.operator.v1alpha1.SupportBundle> informer,
      MixedOperation<
              io.yugabyte.operator.v1alpha1.SupportBundle,
              KubernetesResourceList<io.yugabyte.operator.v1alpha1.SupportBundle>,
              Resource<io.yugabyte.operator.v1alpha1.SupportBundle>>
          resourceClient,
      String namespace,
      Commissioner commissioner,
      TaskExecutor taskExecutor,
      SupportBundleUtil sbu,
      OperatorUtils operatorUtils) {
    this.resourceClient = resourceClient;
    this.informer = informer;
    this.lister = new Lister<>(informer.getIndexer());
    this.namespace = namespace;
    this.commissioner = commissioner;
    this.taskExecutor = taskExecutor;
    this.supportBundleUtil = sbu;
    this.operatorUtils = operatorUtils;
  }

  @Override
  public void run() {
    informer.addEventHandler(this);
    informer.run();
  }

  @Override
  public void onAdd(io.yugabyte.operator.v1alpha1.SupportBundle bundle) {
    try {
      onAddInternal(bundle);
    } catch (Exception e) {
      log.error("Failed to add support bundle", e);
      UUID uuid = new UUID(0, 0);
      markStatus(bundle, SupportBundleStatus.Status.FAILED, uuid);
      return;
    }
  }

  public void onAddInternal(io.yugabyte.operator.v1alpha1.SupportBundle bundle) {
    if (bundle.getStatus() != null && bundle.getStatus().getResourceUUID() != null) {
      // TODO: If we hit this path due to a YBA restart, the bundle won't have its final status
      // update. We need a better way of plugging in the 'status update' function into the tasks.
      log.info("bundle %s is already getting generated", bundle.getStatus().getResourceUUID());
      return;
    }

    log.trace("getting customer to create the support bundle");
    Customer customer;
    try {
      customer = operatorUtils.getOperatorCustomer();
    } catch (Exception e) {
      log.error("failed to get customer", e);
      return;
    }
    // Format start and end dates.
    SimpleDateFormat sdf = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ssZ");
    SupportBundleFormData bundleData = new SupportBundleFormData();
    try {
      bundleData.startDate =
          sdf.parse(
              bundle.getSpec().getCollectionTimerange().getStartDate().replaceAll("Z$", "+0000"));
      if (bundle.getSpec().getCollectionTimerange().getEndDate() != null) {
        bundleData.endDate =
            sdf.parse(
                bundle.getSpec().getCollectionTimerange().getEndDate().replaceAll("Z$", "+0000"));
      } else {
        log.debug("no end date given, setting to current time");
        bundleData.endDate = Date.from(Instant.now());
      }
    } catch (ParseException e) {
      throw new RuntimeException("failed to parse dates", e);
    }

    if (bundle.getSpec().getComponents() == null || bundle.getSpec().getComponents().size() == 0) {
      bundleData.components = EnumSet.allOf(ComponentType.class);
    } else {
      bundleData.components =
          bundle.getSpec().getComponents().stream()
              .map(comp -> ComponentType.valueOf(comp.getValue()))
              .collect(Collectors.toCollection(() -> EnumSet.noneOf(ComponentType.class)));
    }

    // Get the Universe
    Universe universe = null;
    try {
      universe =
          operatorUtils.getUniverseFromNameAndNamespace(
              customer.getId(),
              bundle.getSpec().getUniverseName(),
              bundle.getMetadata().getNamespace());
      if (universe == null) {
        log.error("No universe found with name " + bundle.getSpec().getUniverseName());
        return;
      }
    } catch (Exception e) {
      log.error("Error fetching universe with name " + bundle.getSpec().getUniverseName());
      return;
    }
    SupportBundle supportBundle = SupportBundle.create(bundleData, universe);
    markStatus(bundle, SupportBundleStatus.Status.GENERATING, supportBundle.getBundleUUID());
    SupportBundleTaskParams taskParams =
        new SupportBundleTaskParams(supportBundle, bundleData, customer, universe);
    taskParams.setKubernetesResourceDetails(KubernetesResourceDetails.fromResource(bundle));
    UUID taskUUID = commissioner.submit(TaskType.CreateSupportBundle, taskParams);

    CustomerTask.create(
        customer,
        universe.getUniverseUUID(),
        taskUUID,
        CustomerTask.TargetType.Universe,
        CustomerTask.TaskType.CreateSupportBundle,
        universe.getName());
  }

  @Override
  public void onUpdate(
      io.yugabyte.operator.v1alpha1.SupportBundle oldBundle,
      io.yugabyte.operator.v1alpha1.SupportBundle newBundle) {
    log.warn("updating support bundle is not supported");
  }

  @Override
  public void onDelete(
      io.yugabyte.operator.v1alpha1.SupportBundle bundle, boolean deletedFinalStateUnknown) {
    try {
      onDeleteInternal(bundle, deletedFinalStateUnknown);
    } catch (Exception e) {
      log.error("Failed to delete support bundle", e);
      return;
    }
  }

  public void onDeleteInternal(
      io.yugabyte.operator.v1alpha1.SupportBundle bundle, boolean deletedFinalStateUnknown) {
    UUID bundleUUID = UUID.fromString(bundle.getStatus().getResourceUUID());
    SupportBundle supportBundle = SupportBundle.get(bundleUUID);
    if (supportBundle == null) {
      log.debug("no bundle found");
      return;
    }

    SupportBundle.delete(bundleUUID);

    // Delete the actual archive file
    supportBundleUtil.deleteFile(supportBundle.getPathObject());
  }

  private void markStatus(
      io.yugabyte.operator.v1alpha1.SupportBundle bundle,
      SupportBundleStatus.Status statusEnum,
      UUID uuid) {
    SupportBundleStatus bundleStatus = bundle.getStatus();
    if (bundleStatus == null) {
      bundleStatus = new SupportBundleStatus();
    }
    bundleStatus.setStatus(statusEnum);
    bundle.setStatus(bundleStatus);
    resourceClient.inNamespace(namespace).resource(bundle).replaceStatus();
  }
}
