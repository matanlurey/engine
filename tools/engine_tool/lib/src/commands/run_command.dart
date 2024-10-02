// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io' show ProcessStartMode;

import 'package:engine_build_configs/engine_build_configs.dart';
import 'package:meta/meta.dart';
import 'package:process_runner/process_runner.dart';

import '../build_utils.dart';
import '../flutter_tool_interop/device.dart';
import '../flutter_tool_interop/flutter_tool.dart';
import '../flutter_tool_interop/target_platform.dart';
import '../label.dart';
import '../logger.dart';
import 'command.dart';
import 'flags.dart';

/// The root 'run' command.
final class RunCommand extends CommandBase {
  /// Constructs the 'run' command.
  RunCommand({
    required super.environment,
    required Map<String, BuilderConfig> configs,
    super.help = false,
    super.usageLineLength,
    @visibleForTesting FlutterTool? flutterTool,
  }) {
    // When printing the help/usage for this command, only list all builds
    // when the --verbose flag is supplied.
    final bool includeCiBuilds = environment.verbose || !help;
    builds = runnableBuilds(environment, configs, includeCiBuilds);
    debugCheckBuilds(builds);
    // We default to nothing in order to automatically detect attached devices
    // and select an appropriate target from them.
    addConfigOption(
      environment,
      argParser,
      builds,
      defaultsTo: '',
    );
    addConcurrencyOption(argParser);
    argParser.addFlag(
      rbeFlag,
      defaultsTo: environment.hasRbeConfigInTree(),
      help: 'RBE is enabled by default when available.',
    );

    _flutterTool = flutterTool ?? FlutterTool.fromEnvironment(environment);
  }

  /// Flutter tool.
  late final FlutterTool _flutterTool;

  /// List of compatible builds.
  late final List<Build> builds;

  @override
  String get name => 'run';

  @override
  String get description => '''
Run a Flutter app with a local engine build.
  All arguments after -- are forwarded to flutter run, e.g.:
  et run -- --profile
  et run -- -d macos
See `flutter run --help` for a listing
''';

  Build? _findTargetBuild(String configName) {
    final String demangledName = demangleConfigName(environment, configName);
    return builds
        .where((Build build) => build.name == demangledName)
        .firstOrNull;
  }

  Build? _findHostBuild(Build? targetBuild) {
    if (targetBuild == null) {
      return null;
    }
    final String mangledName = mangleConfigName(environment, targetBuild.name);
    if (mangledName.contains('host_')) {
      return targetBuild;
    }
    // TODO(johnmccutchan): This is brittle, it would be better if we encoded
    // the host config name in the target config.
    final String ci =
        mangledName.startsWith('ci') ? mangledName.substring(0, 3) : '';
    if (mangledName.contains('_debug')) {
      return _findTargetBuild('${ci}host_debug');
    } else if (mangledName.contains('_profile')) {
      return _findTargetBuild('${ci}host_profile');
    } else if (mangledName.contains('_release')) {
      return _findTargetBuild('${ci}host_release');
    }
    return null;
  }

  String _getDeviceId() {
    if (argResults!.rest.contains('-d')) {
      final int index = argResults!.rest.indexOf('-d') + 1;
      if (index < argResults!.rest.length) {
        return argResults!.rest[index];
      }
    }
    if (argResults!.rest.contains('--device-id')) {
      final int index = argResults!.rest.indexOf('--device-id') + 1;
      if (index < argResults!.rest.length) {
        return argResults!.rest[index];
      }
    }
    return '';
  }

  String _getMode() {
    // Sniff the build mode from the args that will be passed to flutter run.
    String mode = 'debug';
    if (argResults!.rest.contains('--profile')) {
      mode = 'profile';
    } else if (argResults!.rest.contains('--release')) {
      mode = 'release';
    }
    return mode;
  }

  late final Future<RunTarget?> _runTarget = (() async {
    final devices = await _flutterTool.devices();
    return RunTarget.detectAndSelect(devices, idPrefix: _getDeviceId());
  })();

  Future<String> _selectTargetConfig() async {
    final configName = argResults![configFlag] as String;
    if (configName.isNotEmpty) {
      return configName;
    }
    final target = await _runTarget;
    if (target == null) {
      return 'host_debug';
    }

    final result = target.buildConfigFor(_getMode());
    environment.logger.status('Building to run on $result');
    return result;
  }

  @override
  Future<int> run() async {
    if (!environment.processRunner.processManager.canRun('flutter')) {
      throw FatalError('Cannot find the "flutter" command in your PATH');
    }

    final configName = await _selectTargetConfig();
    final targetBuild = _findTargetBuild(configName);
    if (targetBuild == null) {
      throw FatalError('Could not find build $configName');
    }

    final hostBuild = _findHostBuild(targetBuild);
    if (hostBuild == null) {
      throw FatalError('Could not find host build for $configName');
    }

    final useRbe = argResults!.flag(rbeFlag);
    if (useRbe && !environment.hasRbeConfigInTree()) {
      throw FatalError('RBE was requested but no RBE config was found');
    }

    final extraGnArgs = [
      if (!useRbe) '--no-rbe',
    ];
    final target = await _runTarget;
    final buildTargetsForShell = target?.buildTargetsForShell ?? [];

    final dashJ = argResults![concurrencyFlag] as String;
    final concurrency = int.tryParse(dashJ);
    if (concurrency == null || concurrency < 0) {
      throw FatalError(
        '--$concurrencyFlag (-j) must specify a positive integer.',
      );
    }

    // First build the host.
    int r = await runBuild(
      environment,
      hostBuild,
      concurrency: concurrency,
      extraGnArgs: extraGnArgs,
      enableRbe: useRbe,
    );
    if (r != 0) {
      throw FatalError('Failed to build host (${hostBuild.name})');
    }

    // Now build the target if it isn't the same.
    if (hostBuild.name != targetBuild.name) {
      r = await runBuild(
        environment,
        targetBuild,
        concurrency: concurrency,
        extraGnArgs: extraGnArgs,
        enableRbe: useRbe,
        targets: buildTargetsForShell,
      );
      if (r != 0) {
        throw FatalError('Failed to build target (${targetBuild.name})');
      }
    }

    final String mangledBuildName =
        mangleConfigName(environment, targetBuild.name);

    final String mangledHostBuildName =
        mangleConfigName(environment, hostBuild.name);

    final List<String> command = <String>[
      'flutter',
      'run',
      '--local-engine-src-path',
      environment.engine.srcDir.path,
      '--local-engine',
      mangledBuildName,
      '--local-engine-host',
      mangledHostBuildName,
      ...argResults!.rest
    ];

    // TODO(johnmccutchan): Be smart and if the user requested a profile
    // config, add the '--profile' flag when invoking flutter run.
    final ProcessRunnerResult result =
        await environment.processRunner.runProcess(
      command,
      runInShell: true,
      startMode: ProcessStartMode.inheritStdio,
    );
    return result.exitCode;
  }
}

/// Metadata about a target to run `flutter run` on.
///
/// This class translates between the `flutter devices` output and the build
/// configurations supported by the engine, including the build targets needed
/// to build the shell for the target platform.
@visibleForTesting
@immutable
final class RunTarget {
  /// Construct a run target from a device returned by `flutter devices`.
  @visibleForTesting
  const RunTarget.fromDevice(this.device);

  /// Device to run on.
  final Device device;

  /// Given a list of devices, returns a build target for the first matching.
  ///
  /// If [idPrefix] is provided, only devices with an id that starts with the
  /// prefix will be considered, otherwise the first device is selected. If no
  /// devices are available, or none match the prefix, `null` is returned.
  @visibleForTesting
  static RunTarget? detectAndSelect(
    Iterable<Device> devices, {
    String idPrefix = '',
  }) {
    if (devices.isEmpty) {
      return null;
    }
    for (final device in devices) {
      if (idPrefix.isNotEmpty) {
        if (device.id.startsWith(idPrefix)) {
          return RunTarget.fromDevice(device);
        }
      }
    }
    if (idPrefix.isNotEmpty) {
      return null;
    }
    return RunTarget.fromDevice(devices.first);
  }

  /// Returns the build configuration for the current platform and given [mode].
  ///
  /// The [mode] is typically one of `debug`, `profile`, or `release`.
  ///
  /// Throws a [FatalError] if the target platform is not supported.
  String buildConfigFor(String mode) {
    return switch (device.targetPlatform) {
      // Supported platforms with known mappings.
      // -----------------------------------------------------------------------
      // ANDROID
      TargetPlatform.androidUnspecified => 'android_$mode',
      TargetPlatform.androidX86 => 'android_${mode}_x86',
      TargetPlatform.androidX64 => 'android_${mode}_x64',
      TargetPlatform.androidArm64 => 'android_${mode}_arm64',

      // DESKTOP (MacOS, Linux, Windows)
      // We do not support cross-builds, so implicitly assume the host platform.
      TargetPlatform.darwinUnspecified ||
      TargetPlatform.darwinX64 ||
      TargetPlatform.linuxX64 ||
      TargetPlatform.windowsX64 =>
        'host_$mode',
      TargetPlatform.darwinArm64 ||
      TargetPlatform.linuxArm64 ||
      TargetPlatform.windowsArm64 =>
        'host_${mode}_arm64',

      // WEB
      TargetPlatform.webJavascript => 'chrome_$mode',

      // Unsupported platforms.
      // -----------------------------------------------------------------------
      // iOS.
      // TODO(matanlurey): https://github.com/flutter/flutter/issues/155960
      TargetPlatform.iOSUnspecified ||
      TargetPlatform.iOSX64 ||
      TargetPlatform.iOSArm64 =>
        throw FatalError(
          'iOS targets are currently unsupported.\n\nIf you are an '
          'iOS engine developer, and have a need for this, please either +1 or '
          'help us implement https://github.com/flutter/flutter/issues/155960.',
        ),

      // LEGACY ANDROID
      TargetPlatform.androidArm => throw FatalError(
          'Legacy Android targets are not supported. '
          'Please use android-arm64 or android-x64.',
        ),

      // FUCHSIA
      TargetPlatform.fuchsiaArm64 ||
      TargetPlatform.fuchsiaX64 =>
        throw FatalError('Fuchsia is not supported.'),

      // TESTER
      TargetPlatform.tester =>
        throw FatalError('flutter_tester is not supported.'),

      // Platforms that maybe could be supported, but we don't know about.
      _ => throw FatalError(
          'Unknown target platform: ${device.targetPlatform.identifier}.\n\nIf '
          'this is a new platform that should be supported, please file a bug: '
          'https://github.com/flutter/flutter/issues/new?labels=e:%20engine-tool.',
        ),
    };
  }

  /// Minimal build targets needed to build the shell for the target platform.
  List<Label> get buildTargetsForShell {
    return switch (device.targetPlatform) {
      // Supported platforms with known mappings.
      // -----------------------------------------------------------------------
      // ANDROID
      TargetPlatform.androidUnspecified ||
      TargetPlatform.androidX86 ||
      TargetPlatform.androidX64 ||
      TargetPlatform.androidArm64 =>
        [Label.parseGn('//flutter/shell/platform/android:android_jar')],

      // iOS.
      TargetPlatform.iOSUnspecified ||
      TargetPlatform.iOSX64 ||
      TargetPlatform.iOSArm64 =>
        [
          Label.parseGn('//flutter/shell/platform/darwin/ios:flutter_framework')
        ],

      // Desktop (MacOS).
      TargetPlatform.darwinUnspecified ||
      TargetPlatform.darwinX64 ||
      TargetPlatform.darwinArm64 =>
        [
          Label.parseGn(
            '//flutter/shell/platform/darwin/macos:flutter_framework',
          )
        ],

      // Desktop (Linux).
      TargetPlatform.linuxX64 || TargetPlatform.linuxArm64 => [
          Label.parseGn(
            '//flutter/shell/platform/linux:flutter_linux_gtk',
          )
        ],

      // Desktop (Windows).
      TargetPlatform.windowsX64 || TargetPlatform.windowsArm64 => [
          Label.parseGn(
            '//flutter/shell/platform/windows',
          )
        ],

      // Web.
      TargetPlatform.webJavascript => [
          Label.parseGn(
            '//flutter/web_sdk:flutter_web_sdk_archive',
          )
        ],

      // Unsupported platforms.
      // -----------------------------------------------------------------------
      _ => throw FatalError(
          'Unknown target platform: ${device.targetPlatform.identifier}.\n\nIf '
          'this is a new platform that should be supported, please file a bug: '
          'https://github.com/flutter/flutter/issues/new?labels=e:%20engine-tool.',
        ),
    };
  }
}
