// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "common/validation.hpp"

#include <limits.h>

#include <algorithm>
#include <cctype>

#include <mesos/resources.hpp>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/constants.hpp>

using std::string;

using google::protobuf::RepeatedPtrField;

namespace mesos {
namespace internal {
namespace common {
namespace validation {

Option<Error> validateID(const string& id)
{
  if (id.empty()) {
    return Error("ID must not be empty");
  }

  if (id.length() > NAME_MAX) {
    return Error(
        "ID must not be greater than " +
        stringify(NAME_MAX) + " characters");
  }

  // The ID cannot be exactly these special path components.
  if (id == "." || id == "..") {
    return Error("'" + id + "' is disallowed");
  }

  // Rules on invalid characters in the ID:
  // - Control characters are obviously not allowed.
  // - Slashes are disallowed as IDs are likely mapped to directories in Mesos.
  auto invalidCharacter = [](char c) {
    return iscntrl(c) ||
           c == os::POSIX_PATH_SEPARATOR ||
           c == os::WINDOWS_PATH_SEPARATOR;
  };

  if (std::any_of(id.begin(), id.end(), invalidCharacter)) {
    return Error("'" + id + "' contains invalid characters");
  }

  return None();
}


// These IDs are valid as long as they meet the common ID requirements
// enforced by `validateID()` but we define each of them separately to
// be clear which IDs are subject to which rules.
Option<Error> validateTaskID(const TaskID& taskId)
{
  return validateID(taskId.value());
}


Option<Error> validateExecutorID(const ExecutorID& executorId)
{
  return validateID(executorId.value());
}


Option<Error> validateSlaveID(const SlaveID& slaveId)
{
  return validateID(slaveId.value());
}


Option<Error> validateFrameworkID(const FrameworkID& frameworkId)
{
  return validateID(frameworkId.value());
}


Option<Error> validateSecret(const Secret& secret)
{
  switch (secret.type()) {
    case Secret::REFERENCE:
      if (!secret.has_reference()) {
        return Error(
            "Secret of type REFERENCE must have the 'reference' field set");
      }

      if (secret.has_value()) {
        return Error(
            "Secret '" + secret.reference().name() + "' of type REFERENCE "
            "must not have the 'value' field set");
      }
      break;

    case Secret::VALUE:
      if (!secret.has_value()) {
        return Error("Secret of type VALUE must have the 'value' field set");
      }

      if (secret.has_reference()) {
        return Error(
            "Secret of type VALUE must not have the 'reference' field set");
      }
      break;

    case Secret::UNKNOWN:
      break;

    UNREACHABLE();
  }

  return None();
}


Option<Error> validateEnvironment(const Environment& environment)
{
  foreach (const Environment::Variable& variable, environment.variables()) {
    switch (variable.type()) {
      case Environment::Variable::SECRET: {
        if (!variable.has_secret()) {
          return Error(
              "Environment variable '" + variable.name() +
              "' of type 'SECRET' must have a secret set");
        }

        if (variable.has_value()) {
          return Error(
              "Environment variable '" + variable.name() +
              "' of type 'SECRET' must not have a value set");
        }

        Option<Error> error = validateSecret(variable.secret());
        if (error.isSome()) {
          return Error(
              "Environment variable '" + variable.name() + "' specifies an "
              "invalid secret: " + error->message);
        }

        if (variable.secret().value().data().find('\0') != string::npos) {
            return Error(
                "Environment variable '" + variable.name() + "' specifies a "
                "secret containing null bytes, which is not allowed in the "
                "environment");
        }
        break;
      }

      // NOTE: If new variable types are added in the future and an upgraded
      // client/master sends a new type to an older master/agent, the older
      // master/agent will see VALUE instead of the new type, since VALUE is set
      // as the default type in the protobuf definition.
      case Environment::Variable::VALUE:
        if (!variable.has_value()) {
          return Error(
              "Environment variable '" + variable.name() +
              "' of type 'VALUE' must have a value set");
        }

        if (variable.has_secret()) {
          return Error(
              "Environment variable '" + variable.name() +
              "' of type 'VALUE' must not have a secret set");
        }
        break;

      case Environment::Variable::UNKNOWN:
          return Error("Environment variable of type 'UNKNOWN' is not allowed");

      UNREACHABLE();
    }
  }

  return None();
}


// TODO(greggomann): Do more than just validate the `Environment`.
Option<Error> validateCommandInfo(const CommandInfo& command)
{
  return validateEnvironment(command.environment());
}


Option<Error> validateVolume(const Volume& volume)
{
  // TODO(jieyu): Add a validation for path.

  // Only one of the following fields can be set:
  //   1. host_path
  //   2. image
  //   3. source
  int count = 0;
  if (volume.has_host_path()) { count++; }
  if (volume.has_image()) { count++; }
  if (volume.has_source()) { count++; }

  if (count != 1) {
    return Error(
        "Only one of them should be set: "
        "'host_path', 'image' and 'source'");
  }

  if (volume.has_source()) {
    switch (volume.source().type()) {
      case Volume::Source::DOCKER_VOLUME:
        if (!volume.source().has_docker_volume()) {
          return Error(
              "'source.docker_volume' is not set for DOCKER_VOLUME volume");
        }
        break;
      case Volume::Source::HOST_PATH:
        if (!volume.source().has_host_path()) {
          return Error(
              "'source.host_path' is not set for HOST_PATH volume");
        }
        break;
      case Volume::Source::SANDBOX_PATH:
        if (!volume.source().has_sandbox_path()) {
          return Error(
              "'source.sandbox_path' is not set for SANDBOX_PATH volume");
        }
        break;
      case Volume::Source::SECRET:
        if (!volume.source().has_secret()) {
          return Error(
              "'source.secret' is not set for SECRET volume");
        }
        break;
      default:
        return Error("'source.type' is unknown");
    }
  }

  return None();
}


Option<Error> validateContainerInfo(const ContainerInfo& containerInfo)
{
  foreach (const Volume& volume, containerInfo.volumes()) {
    Option<Error> error = validateVolume(volume);
    if (error.isSome()) {
      return Error("Invalid volume: " + error->message);
    }
  }

  if (containerInfo.type() == ContainerInfo::DOCKER) {
    if (!containerInfo.has_docker()) {
      return Error(
          "DockerInfo 'docker' is not set for DOCKER typed ContainerInfo");
    }

    // We do not support setting `name` parameter in Docker info because
    // Docker containerizer has its own way to name the Docker container,
    // otherwise Docker containerizer will not be able to recognize the
    // created container, see MESOS-8497 for details.
    foreach (const Parameter& parameter,
             containerInfo.docker().parameters()) {
      if (parameter.key() == "name") {
        return Error("Parameter in DockerInfo must not be 'name'");
      }
    }
  }

  return None();
}


// Validates that the `gpus` resource is not fractional.
// We rely on scalar resources only having 3 digits of precision.
Option<Error> validateGpus(const RepeatedPtrField<Resource>& resources)
{
  double gpus = Resources(resources).gpus().getOrElse(0.0);
  if (static_cast<long long>(gpus * 1000.0) % 1000 != 0) {
    return Error("The 'gpus' resource must be an unsigned integer");
  }

  return None();
}


Option<Error> validateHealthCheck(const HealthCheck& healthCheck)
{
  if (!healthCheck.has_type()) {
    return Error("HealthCheck must specify 'type'");
  }

  switch (healthCheck.type()) {
    case HealthCheck::COMMAND: {
      if (!healthCheck.has_command()) {
        return Error("Expecting 'command' to be set for COMMAND health check");
      }

      const CommandInfo& command = healthCheck.command();

      if (!command.has_value()) {
        string commandType =
          (command.shell() ? "'shell command'" : "'executable path'");

        return Error("Command health check must contain " + commandType);
      }

      Option<Error> error =
        common::validation::validateCommandInfo(command);
      if (error.isSome()) {
        return Error(
            "Health check's `CommandInfo` is invalid: " + error->message);
      }

      // TODO(alexr): Make sure irrelevant fields, e.g., `uris` are not set.

      break;
    }
    case HealthCheck::HTTP: {
      if (!healthCheck.has_http()) {
        return Error("Expecting 'http' to be set for HTTP health check");
      }

      const HealthCheck::HTTPCheckInfo& http = healthCheck.http();

      if (http.has_scheme() &&
          http.scheme() != "http" &&
          http.scheme() != "https") {
        return Error(
            "Unsupported HTTP health check scheme: '" + http.scheme() + "'");
      }

      if (http.has_path() && !strings::startsWith(http.path(), '/')) {
        return Error(
            "The path '" + http.path() +
            "' of HTTP health check must start with '/'");
      }

      break;
    }
    case HealthCheck::TCP: {
      if (!healthCheck.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP health check");
      }

      break;
    }
    case HealthCheck::UNKNOWN: {
      return Error(
          "'" + HealthCheck::Type_Name(healthCheck.type()) + "'"
          " is not a valid health check type");
    }
  }

  if (healthCheck.has_delay_seconds() && healthCheck.delay_seconds() < 0.0) {
    return Error("Expecting 'delay_seconds' to be non-negative");
  }

  if (healthCheck.has_grace_period_seconds() &&
      healthCheck.grace_period_seconds() < 0.0) {
    return Error("Expecting 'grace_period_seconds' to be non-negative");
  }

  if (healthCheck.has_interval_seconds() &&
      healthCheck.interval_seconds() < 0.0) {
    return Error("Expecting 'interval_seconds' to be non-negative");
  }

  if (healthCheck.has_timeout_seconds() &&
      healthCheck.timeout_seconds() < 0.0) {
    return Error("Expecting 'timeout_seconds' to be non-negative");
  }

  return None();
}


Option<Error> validateCheckInfo(const CheckInfo& checkInfo)
{
  if (!checkInfo.has_type()) {
    return Error("CheckInfo must specify 'type'");
  }

  switch (checkInfo.type()) {
    case CheckInfo::COMMAND: {
      if (!checkInfo.has_command()) {
        return Error("Expecting 'command' to be set for COMMAND check");
      }

      const CommandInfo& command = checkInfo.command().command();

      if (!command.has_value()) {
        string commandType =
          (command.shell() ? "'shell command'" : "'executable path'");

        return Error("Command check must contain " + commandType);
      }

      Option<Error> error =
        common::validation::validateCommandInfo(command);
      if (error.isSome()) {
        return Error(
            "Check's `CommandInfo` is invalid: " + error->message);
      }

      // TODO(alexr): Make sure irrelevant fields, e.g., `uris` are not set.

      break;
    }
    case CheckInfo::HTTP: {
      if (!checkInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check");
      }

      const CheckInfo::Http& http = checkInfo.http();

      if (http.has_path() && !strings::startsWith(http.path(), '/')) {
        return Error(
            "The path '" + http.path() + "' of HTTP check must start with '/'");
      }

      break;
    }
    case CheckInfo::TCP: {
      if (!checkInfo.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP check");
      }

      break;
    }
    case CheckInfo::UNKNOWN: {
      return Error(
          "'" + CheckInfo::Type_Name(checkInfo.type()) + "'"
          " is not a valid check type");
    }
  }

  if (checkInfo.has_delay_seconds() && checkInfo.delay_seconds() < 0.0) {
    return Error("Expecting 'delay_seconds' to be non-negative");
  }

  if (checkInfo.has_interval_seconds() && checkInfo.interval_seconds() < 0.0) {
    return Error("Expecting 'interval_seconds' to be non-negative");
  }

  if (checkInfo.has_timeout_seconds() && checkInfo.timeout_seconds() < 0.0) {
    return Error("Expecting 'timeout_seconds' to be non-negative");
  }

  return None();
}


Option<Error> validateCheckStatusInfo(const CheckStatusInfo& checkStatusInfo)
{
  if (!checkStatusInfo.has_type()) {
    return Error("CheckStatusInfo must specify 'type'");
  }

  switch (checkStatusInfo.type()) {
    case CheckInfo::COMMAND: {
      if (!checkStatusInfo.has_command()) {
        return Error(
            "Expecting 'command' to be set for COMMAND check's status");
      }
      break;
    }
    case CheckInfo::HTTP: {
      if (!checkStatusInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check's status");
      }
      break;
    }
    case CheckInfo::TCP: {
      if (!checkStatusInfo.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP check's status");
      }
      break;
    }
    case CheckInfo::UNKNOWN: {
      return Error(
          "'" + CheckInfo::Type_Name(checkStatusInfo.type()) + "'"
          " is not a valid check's status type");
    }
  }

  return None();
}

} // namespace validation {
} // namespace common {
} // namespace internal {
} // namespace mesos {
