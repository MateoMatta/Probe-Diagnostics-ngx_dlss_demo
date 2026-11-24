param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$MediaPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [ValidateRange(8, 120)][int]$SecondsPerCase = 12,
    [switch]$ShaderBindingsOnly,
    [switch]$OverlapOnly,
    [switch]$IdentityOnly,
    [switch]$DxgiAdapterOnly,
    [switch]$ResizeOnly,
    [switch]$GpuPipelineOnly,
    [switch]$CameraOnly,
    [switch]$ResizeDlssOnly,
    [switch]$StressFps
)

$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$media = (Resolve-Path -LiteralPath $MediaPath).Path
$workingDirectory = Split-Path -Parent $executablePath
foreach ($required in @('dxgi.dll', 'OptiScaler.addon64', 'OptiScaler.ini')) {
    if (-not (Test-Path -LiteralPath (Join-Path $workingDirectory $required) -PathType Leaf)) {
        throw "Missing $required in $workingDirectory"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $media 'sponza.json') -PathType Leaf)) {
    throw "Media path has no sponza.json: $media"
}
if (Test-Path -LiteralPath $OutputPath) {
    throw "Output path already exists: $OutputPath"
}
New-Item -ItemType Directory -Path $OutputPath | Out-Null
$output = (Resolve-Path -LiteralPath $OutputPath).Path

$environmentNames = @('OPTISCALER_DEMO_TRACE', 'OPTISCALER_DEMO_READBACK',
    'OPTISCALER_PROBE_DETAIL_START', 'OPTISCALER_PROBE_ALL_TARGET_BINDINGS',
    'OPTISCALER_PROBE_SHADER_BINDINGS', 'OPTISCALER_DEMO_GPU_SYNC_TEST',
    'OPTISCALER_DEMO_IDENTITY_SELFTEST', 'OPTISCALER_PROBE_RESOURCE_LIFETIMES',
    'OPTISCALER_DEMO_GPU_PIPELINE_TEST', 'DONUT_MEDIA_PATH')
$previousEnvironment = @{}
foreach ($name in $environmentNames) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

$cases = @(
    @{ Name = 'static-temporal'; Args = '-aamode temporal'; Readback = $true },
    @{ Name = 'camera-temporal'; Args = '-aamode temporal -diagnostic-camera'; Readback = $true },
    @{ Name = 'objects-baseline'; Args = '-aamode temporal -diagnostic-objects'; Readback = $true },
    @{ Name = 'objects-temporal'; Args = '-aamode temporal -diagnostic-objects -diagnostic-object-ids'; Readback = $true },
    @{ Name = 'objects-dlss-camera'; Args = '-aamode dlss -diagnostic-camera -diagnostic-objects -diagnostic-object-ids'; Readback = $true },
    @{ Name = 'coplanar-overlap'; Args = '-aamode temporal -diagnostic-objects -diagnostic-object-ids -diagnostic-overlap-selftest'; Readback = $true },
    @{ Name = 'identity-selftest'; Args = '-aamode temporal'; Readback = $false },
    @{ Name = 'stencil-reset'; Args = '-aamode dlss -diagnostic-camera -diagnostic-objects -diagnostic-clear-stencil -diagnostic-history-resets'; Readback = $true },
    @{ Name = 'mode-transitions'; Args = '-diagnostic-cycle -diagnostic-objects'; Readback = $false },
    @{ Name = 'resource-resize'; Args = '-aamode temporal -diagnostic-resize'; Readback = $false },
    @{ Name = 'resource-resize-dlss'; Args = '-aamode dlss -diagnostic-resize'; Readback = $false },
    @{ Name = 'gpu-pipeline'; Args = '-aamode temporal'; Readback = $false },
    @{ Name = 'frame-correlation'; Args = '-aamode dlss'; Readback = $false },
    @{ Name = 'all-target-bindings'; Args = '-aamode dlss'; Readback = $false },
    @{ Name = 'shader-bindings'; Args = '-aamode temporal'; Readback = $false }
)
if (@($ShaderBindingsOnly, $OverlapOnly, $IdentityOnly, $DxgiAdapterOnly, $ResizeOnly, $ResizeDlssOnly, $GpuPipelineOnly, $CameraOnly | Where-Object { $_ }).Count -gt 1) {
    throw 'Select only one targeted harness mode'
}
if ($ShaderBindingsOnly) {
    $cases = @($cases | Where-Object { $_.Name -eq 'shader-bindings' })
}
if ($OverlapOnly) {
    $cases = @($cases | Where-Object { $_.Name -eq 'coplanar-overlap' })
}
if ($IdentityOnly) {
    $cases = @($cases | Where-Object { $_.Name -eq 'identity-selftest' })
}
if ($DxgiAdapterOnly) {
    $cases = @(@{ Name = 'static-temporal'; Args = '-aamode temporal -diagnostic-dxgi-adapter'; Readback = $true })
}
if ($ResizeOnly) {
    $cases = @($cases | Where-Object { $_.Name -eq 'resource-resize' })
}
if ($ResizeDlssOnly) {
    $cases = @($cases | Where-Object { $_.Name -eq 'resource-resize-dlss' })
}
if ($GpuPipelineOnly) {
    $cases = @($cases | Where-Object { $_.Name -eq 'gpu-pipeline' })
}
if ($CameraOnly) {
    $cases = @($cases | Where-Object { $_.Name -eq 'camera-temporal' })
}

try {
    $baselineHashes = @{}
    [Environment]::SetEnvironmentVariable('OPTISCALER_DEMO_TRACE', '1', 'Process')
    [Environment]::SetEnvironmentVariable('OPTISCALER_PROBE_DETAIL_START', $null, 'Process')
    [Environment]::SetEnvironmentVariable('DONUT_MEDIA_PATH', $media, 'Process')
    foreach ($case in $cases) {
        $readback = $null
        if ($case.Readback) { $readback = '1' }
        [Environment]::SetEnvironmentVariable('OPTISCALER_DEMO_READBACK', $readback, 'Process')
        $detailStart = $null
        if ($case.Name -eq 'frame-correlation') { $detailStart = '115' }
        [Environment]::SetEnvironmentVariable('OPTISCALER_PROBE_DETAIL_START', $detailStart, 'Process')
        $allBindings = $null
        if ($case.Name -eq 'all-target-bindings') { $allBindings = '1' }
        [Environment]::SetEnvironmentVariable('OPTISCALER_PROBE_ALL_TARGET_BINDINGS', $allBindings, 'Process')
        $shaderBindings = $null
        if ($case.Name -eq 'shader-bindings') { $shaderBindings = '1' }
        [Environment]::SetEnvironmentVariable('OPTISCALER_PROBE_SHADER_BINDINGS', $shaderBindings, 'Process')
        [Environment]::SetEnvironmentVariable('OPTISCALER_DEMO_GPU_SYNC_TEST', $shaderBindings, 'Process')
        $identity = $null
        if ($case.Name -eq 'identity-selftest') { $identity = '1' }
        [Environment]::SetEnvironmentVariable('OPTISCALER_DEMO_IDENTITY_SELFTEST', $identity, 'Process')
        $lifetimes = $null
        if ($case.Name -in @('resource-resize', 'resource-resize-dlss')) { $lifetimes = '1' }
        [Environment]::SetEnvironmentVariable('OPTISCALER_PROBE_RESOURCE_LIFETIMES', $lifetimes, 'Process')
        $pipeline = $null
        if ($case.Name -eq 'gpu-pipeline') { $pipeline = '1' }
        [Environment]::SetEnvironmentVariable('OPTISCALER_DEMO_GPU_PIPELINE_TEST', $pipeline, 'Process')
        $caseDirectory = Join-Path $output $case.Name
        New-Item -ItemType Directory -Path $caseDirectory | Out-Null
        $existingLogs = @{}
        foreach ($pattern in @('TemporalInputs_*.log', 'OptiScaler-ReShade_*.log')) {
            Get-ChildItem -LiteralPath $workingDirectory -Filter $pattern | ForEach-Object {
                $existingLogs[$_.FullName] = $true
            }
        }
        $stressArgument = if ($StressFps) { ' -stress-fps' } else { '' }
        $process = Start-Process -FilePath $executablePath -WorkingDirectory $workingDirectory `
            -ArgumentList "-d3d11 -width 640 -height 360 $($case.Args)$stressArgument" -PassThru
        try {
        $startup = [Diagnostics.Stopwatch]::StartNew()
        do {
            $process.Refresh()
            if ($process.HasExited) { throw "$($case.Name): early exit $($process.ExitCode)" }
            if ($process.MainWindowHandle -ne 0) { break }
            Start-Sleep -Milliseconds 250
        } while ($startup.Elapsed.TotalSeconds -lt 30)
        if ($process.MainWindowHandle -eq 0) { throw "$($case.Name): main window did not appear within 30 seconds" }
        Start-Sleep -Seconds $SecondsPerCase
        $process.Refresh()
        if ($process.HasExited) { throw "$($case.Name): early exit $($process.ExitCode)" }
        if (-not $process.CloseMainWindow()) { throw "$($case.Name): could not request close, PID $($process.Id)" }
        if (-not $process.WaitForExit(20000)) { throw "$($case.Name): close timed out, PID $($process.Id)" }
        if ($process.ExitCode -ne 0) { throw "$($case.Name): exit $($process.ExitCode)" }
        foreach ($pattern in @("TemporalInputs_$($process.Id)_*.log", "OptiScaler-ReShade_$($process.Id)_*.log")) {
            $files = @(Get-ChildItem -LiteralPath $workingDirectory -Filter $pattern |
                Where-Object { -not $existingLogs.ContainsKey($_.FullName) })
            if ($files.Count -ne 1) { throw "$($case.Name): expected one new $pattern, found $($files.Count)" }
            Copy-Item -LiteralPath $files[0].FullName -Destination $caseDirectory
        }
        $reshadeLog = Join-Path $workingDirectory 'ReShade.log'
        if (Test-Path -LiteralPath $reshadeLog) { Copy-Item -LiteralPath $reshadeLog -Destination $caseDirectory }
        $trace = Get-Content -LiteralPath (Get-ChildItem -LiteralPath $caseDirectory -Filter 'TemporalInputs_*.log').FullName
        $expectedLimit = if ($StressFps) { 300 } else { 240 }
        $minimumInterval = [math]::Ceiling(1000000000.0 / $expectedLimit)
        $pacing = @($trace | Where-Object { $_ -match 'event=frame_pacing limit=(\d+) interval_ns=(\d+)' })
        if ($pacing.Count -lt 100) { throw "$($case.Name): insufficient frame pacing samples ($($pacing.Count))" }
        foreach ($line in $pacing) {
            if ($line -notmatch 'event=frame_pacing limit=(\d+) interval_ns=(\d+)' -or
                [int]$Matches[1] -ne $expectedLimit -or [long]$Matches[2] -lt $minimumInterval) {
                throw "$($case.Name): invalid frame pacing sample: $line"
            }
        }
        $addonPath = (Get-ChildItem -LiteralPath $caseDirectory -Filter 'OptiScaler-ReShade_*.log').FullName
        $addon = if ($case.Name -eq 'shader-bindings') { $null } else { Get-Content -LiteralPath $addonPath }
        if ($case.Name -in @('resource-resize', 'resource-resize-dlss')) {
            if (-not @($addon | Where-Object { $_ -match 'event=resource_lifetime_capture mode=enabled' }).Count -or
                @($addon | Where-Object { $_ -match 'event=resource_lifetime_error\b' }).Count) {
                throw "$($case.Name): resource lifetime capture unavailable or reported an error"
            }
            $live = @{}
            $seenGenerations = @{}
            $lifetimesByGeneration = @{}
            $lastSequence = 0L
            $creates = 0
            $destroys = 0
            $observed = 0
            $unknown = 0
            foreach ($line in $addon) {
                if ($line -match 'event=resource_lifetime action=(create|destroy|observe) sequence=(\d+) (?:bind_sequence=\d+ )?device=([0-9A-Fa-f]+) resource=0x([0-9A-Fa-f]+) generation=(\d+)') {
                    $action = $Matches[1]; $sequence = [long]$Matches[2]
                    $key = "$($Matches[3]):$($Matches[4])"; $generation = $Matches[5]
                    if ($sequence -le $lastSequence) { throw "$($case.Name): lifetime sequence out of order" }
                    $lastSequence = $sequence
                    if ($action -eq 'create') {
                        if ($live.ContainsKey($key) -or $seenGenerations.ContainsKey($generation) -or $generation -eq '0') {
                            throw "$($case.Name): duplicate live resource or generation"
                        }
                        $live[$key] = $generation; $seenGenerations[$generation] = $true; ++$creates
                        if ($line -notmatch '\bqpc_valid=1 qpc=(\d+)\b') { throw "$($case.Name): missing creation QPC" }
                        $lifetimesByGeneration[$generation] = @{
                            Resource = $key.Split(':')[1].TrimStart('0').ToUpperInvariant()
                            Created = [long]$Matches[1]; Destroyed = $null
                        }
                    } elseif ($action -eq 'destroy') {
                        if (-not $live.ContainsKey($key) -or $live[$key] -ne $generation) {
                            throw "$($case.Name): destruction does not match live generation"
                        }
                        if ($line -notmatch '\bqpc_valid=1 qpc=(\d+)\b') { throw "$($case.Name): missing destruction QPC" }
                        $lifetimesByGeneration[$generation].Destroyed = [long]$Matches[1]
                        $live.Remove($key); ++$destroys
                    } elseif ($generation -eq '0') {
                        if ($live.ContainsKey($key) -or $line -notmatch '\bstatus=unknown\b') {
                            throw "$($case.Name): invalid unknown observation"
                        }
                        ++$unknown
                    } else {
                        if (-not $live.ContainsKey($key) -or $live[$key] -ne $generation -or $line -notmatch '\bstatus=live\b') {
                            throw "$($case.Name): observation does not match live generation"
                        }
                        ++$observed
                    }
                }
            }
            if ($creates -lt 2 -or $destroys -lt 2 -or $observed -lt 2) {
                throw "$($case.Name): insufficient lifecycle coverage: create=$creates destroy=$destroys observed=$observed"
            }
            if ($live.Count -ne 0 -or -not @($addon | Where-Object {
                $_ -match 'event=resource_lifetime_device_end\b.*\bremaining=0\b'
            }).Count) {
                throw "$($case.Name): tracked resources did not close cleanly with the device"
            }
            foreach ($step in @(1, 2)) {
                if (@($trace | Where-Object { $_ -match "event=resize_request step=$step\b" }).Count -ne 1) {
                    throw "$($case.Name): missing resize request step $step"
                }
            }
            $mode = if ($case.Name -eq 'resource-resize-dlss') { 'DLSS' } else { 'TemporalAA' }
            $small = @($trace | Where-Object { $_ -match "event=frame mode=$mode\b.*display=640x360\b" })
            $large = @($trace | Where-Object { $_ -match "event=frame mode=$mode\b.*display=800x450\b" })
            $resizeBack = @($trace | Where-Object { $_ -match 'event=resize_request step=2\b' })[0]
            if ($resizeBack -notmatch '\bqpc=(\d+)') { throw "$($case.Name): missing resize timestamp" }
            $resizeBackQpc = [long]$Matches[1]
            $smallAfter = @($small | Where-Object {
                $_ -match '\bqpc=(\d+)' -and [long]$Matches[1] -gt $resizeBackQpc
            })
            if ($small.Count -lt 2 -or $large.Count -lt 1 -or $large[0] -notmatch 'previous_valid=0\b' -or
                $smallAfter.Count -lt 2 -or $smallAfter[0] -notmatch 'previous_valid=0\b' -or
                $smallAfter[1] -notmatch 'previous_valid=1\b' -or
                $large.Count -lt 2 -or $large[1] -notmatch 'previous_valid=1\b') {
                throw "$($case.Name): resize sizes or history invalidation not observed"
            }
            foreach ($pair in @(@($large[0], $large[1]), @($smallAfter[0], $smallAfter[1]))) {
                if ($pair[0] -notmatch '\bframe=(\d+)' ) { throw "$($case.Name): missing invalidation frame" }
                $firstFrame = [long]$Matches[1]
                if ($pair[1] -notmatch '\bframe=(\d+)' -or [long]$Matches[1] -ne $firstFrame + 1) {
                    throw "$($case.Name): history did not recover on the next sampled frame"
                }
                if ($mode -eq 'DLSS') {
                    $recoveryFrame = $Matches[1]
                    if (@($trace | Where-Object {
                        $_ -match "\bframe=$recoveryFrame event=ngx_dx11_evaluate_inputs\b.*\breset=0\b"
                    }).Count -ne 1) {
                        throw "$($case.Name): NGX reset did not clear on recovery frame $recoveryFrame"
                    }
                }
            }
            $previousGenerations = @{}
            $event = if ($mode -eq 'DLSS') { 'ngx_dx11_evaluate_inputs' } else { 'taa_inputs' }
            $roles = if ($mode -eq 'DLSS') { @('color', 'output', 'motion', 'depth') } else {
                @('resolve_color', 'output', 'motion', 'motion_pass_depth', 'feedback1', 'feedback2')
            }
            foreach ($frameLine in @($small[0], $large[0], $smallAfter[0])) {
                if ($frameLine -notmatch '\bframe=(\d+)' ) { throw "$($case.Name): missing frame ID" }
                $frameId = $Matches[1]
                $inputs = @($trace | Where-Object { $_ -match "\bframe=$frameId event=$event\b" })
                if ($inputs.Count -ne 1 -or $inputs[0] -notmatch '\bqpc=(\d+)') {
                    throw "$($case.Name): missing temporal input timestamp at frame $frameId"
                }
                $inputQpc = [long]$Matches[1]
                if ($mode -eq 'DLSS' -and $inputs[0] -notmatch '\breset=1\b') {
                    throw "$($case.Name): NGX reset missing at frame $frameId"
                }
                foreach ($role in $roles) {
                    if ($inputs[0] -notmatch "\b$role=([0-9A-Fa-f]+)") { throw "$($case.Name): missing $role" }
                    $resource = $Matches[1].TrimStart('0').ToUpperInvariant()
                    $matching = @($lifetimesByGeneration.Keys | Where-Object {
                        $life = $lifetimesByGeneration[$_]
                        $life.Resource -eq $resource -and $life.Created -le $inputQpc -and
                            ($null -eq $life.Destroyed -or $inputQpc -lt $life.Destroyed)
                    })
                    if ($matching.Count -ne 1) { throw "$($case.Name): $role at frame $frameId has $($matching.Count) live generations" }
                    if ($previousGenerations.ContainsKey($role) -and $previousGenerations[$role] -eq $matching[0]) {
                        throw "$($case.Name): $role was not recreated on resize"
                    }
                    $previousGenerations[$role] = $matching[0]
                }
            }
            Write-Output "RESOURCE_LIFETIMES create=$creates destroy=$destroys live_observations=$observed unknown_observations=$unknown"
            Write-Output "RESOURCE_CORRELATION three_resolutions=1 roles=$($roles.Count) mode=$mode history_recovery=next_frame"
        }
        if ($case.Name -ne 'shader-bindings' -and
            -not @($addon | Where-Object { $_ -match 'event=logger_shutdown dropped=0 file_write_failed=0 .*truncated=0' }).Count) {
            throw "$($case.Name): missing clean logger shutdown"
        }
        if (@($trace | Where-Object {
            $_ -match 'event=(readback_error|stencil_clear_error|taa_binding_error|gpu_sync_error|temporal_parameter_error)\b' -or
            $_ -match 'event=resource_contract(?:_selftest)?\b.*\bstatus=error\b'
        }).Count) {
            throw "$($case.Name): diagnostic error; inspect archived trace"
        }
        if ($case.Name -eq 'gpu-pipeline') {
            $submissions = @($trace | Where-Object { $_ -match 'event=gpu_pipeline_submit\b' })
            $completions = @($trace | Where-Object { $_ -match 'event=gpu_pipeline_complete\b' })
            $results = @($trace | Where-Object { $_ -match 'event=gpu_pipeline_result submitted=8 completed=8 max_outstanding=(\d+)' })
            if ($submissions.Count -ne 8 -or $completions.Count -ne 8 -or $results.Count -ne 1 -or
                $results[0] -notmatch 'max_outstanding=(\d+)' -or [int]$Matches[1] -lt 3) {
                throw "$($case.Name): incomplete multi-frame GPU query experiment"
            }
            for ($i = 0; $i -lt 8; ++$i) {
                $expectedFrame = 120 + $i
                if ($submissions[$i] -notmatch "\bsubmitted_frame=$expectedFrame\b" -or
                    $completions[$i] -notmatch "\bsubmitted_frame=$expectedFrame observed_frame=(\d+)\b" -or
                    [long]$Matches[1] -lt $expectedFrame) {
                    throw "$($case.Name): invalid query ordering for frame $expectedFrame"
                }
            }
            Write-Output "GPU_PIPELINE submitted=8 completed=8 minimum_outstanding=3"
        }
        $evaluations = @($trace | Where-Object { $_ -match '\bevent=(taa_inputs|ngx_dx11_evaluate_inputs)\b' })
        $sampledFrames = @($trace | Where-Object { $_ -match '\bevent=frame mode=' })
        $frameTimings = @($trace | Where-Object { $_ -match '\bevent=frame_timing frame_start_interval_ns=\d+ animation_delta_seconds=' })
        if ($sampledFrames.Count -eq 0 -or $frameTimings.Count -ne $sampledFrames.Count) {
            throw "$($case.Name): missing sampled frame timing"
        }
        if ($evaluations.Count -gt 0) {
            if (@($trace | Where-Object { $_ -match 'event=resource_contract_selftest status=pass cases=5\b' }).Count -ne 1) {
                throw "$($case.Name): resource contract self-test missing or failed"
            }
            $contracts = @{}
            foreach ($line in $trace) {
                if ($line -match '\bframe=(\d+) event=resource_contract path=(taa|dlss) role=(\w+)\b.*\bstatus=(pass|absent_optional)\b') {
                    $key = "$($Matches[1]):$($Matches[2]):$($Matches[3])"
                    if ($contracts.ContainsKey($key)) { throw "$($case.Name): duplicate resource contract $key" }
                    $contracts[$key] = $Matches[4]
                }
            }
            foreach ($line in $evaluations) {
                if ($line -notmatch '\bframe=(\d+) event=(taa_inputs|ngx_dx11_evaluate_inputs)\b') {
                    throw "$($case.Name): invalid evaluation line"
                }
                $frame = $Matches[1]
                $path = if ($Matches[2] -eq 'taa_inputs') { 'taa' } else { 'dlss' }
                $roles = if ($path -eq 'taa') { @('color', 'depth', 'motion', 'output', 'feedback1', 'feedback2') } else { @('color', 'depth', 'motion', 'output', 'exposure') }
                foreach ($role in $roles) {
                    $key = "${frame}:${path}:${role}"
                    if (-not $contracts.ContainsKey($key) -or
                        ($contracts[$key] -ne 'pass' -and -not ($role -eq 'exposure' -and $contracts[$key] -eq 'absent_optional'))) {
                        throw "$($case.Name): missing valid resource contract $key"
                    }
                }
            }
        }
        if ($case.Name -eq 'identity-selftest') {
            $identityResults = @($trace | Where-Object { $_ -match '\bframe=120 event=identity_selftest\b' })
            if ($identityResults.Count -ne 1 -or
                $identityResults[0] -notmatch '\bstatus=pass cases=14 pixels=106496 mismatches=0 hr=0x00000000\b') {
                throw "$($case.Name): identity self-test missing or failed: $identityResults"
            }
        }
        if ($case.Readback -and -not @($trace | Where-Object { $_ -match 'event=readback_selftest status=pass' }).Count) {
            throw "$($case.Name): GPU readback self-test missing or failed"
        }
        $checks = @($trace | Where-Object { $_ -match 'event=motion_reprojection_check scope=' })
        if ($case.Name -eq 'static-temporal' -and
            ($checks.Count -lt 3 -or @($checks | Where-Object { $_ -notmatch 'compared=256' -or $_ -notmatch '\bmismatches=0\b' }).Count)) {
            throw "$($case.Name): static reprojection validation failed"
        }
        if ($case.Name -eq 'camera-temporal') {
            if ($checks.Count -lt 3 -or @($checks | Where-Object {
                $_ -notmatch 'compared=256' -or $_ -notmatch '\bmismatches=0\b' -or
                $_ -notmatch '\bmax_expected=([0-9.eE+-]+)' -or
                [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) -le 0.02
            }).Count) {
                throw "$($case.Name): camera-motion reprojection validation failed"
            }
            $controls = @($trace | Where-Object { $_ -match 'event=motion_convention_controls\b' })
            if ($controls.Count -lt 3) { throw "$($case.Name): missing motion convention controls" }
            foreach ($line in $controls) {
                if ($line -notmatch '\binformative=(\d+) reversed_rejected=(\d+) normalized_rejected=(\d+)' -or
                    [int]$Matches[1] -lt 20 -or [int]$Matches[2] -lt 20 -or [int]$Matches[3] -lt 20) {
                    throw "$($case.Name): insufficient evidence against reversed or normalized motion: $line"
                }
            }
        }
        if ($case.Name -like 'objects-*') {
            $models = @($trace | Where-Object { $_ -match 'event=object_motion_models' })
            foreach ($index in @(0, 1)) {
                if (-not @($models | Where-Object { $_ -match "matches$index=[1-9]\d*\b" }).Count) {
                    throw "$($case.Name): no distinguishable matches for object model $index"
                }
                if ($case.Name -ne 'objects-baseline') {
                    $ids = @($trace | Where-Object { $_ -match '\bframe=120 event=object_id_motion_check\b' })
                    if ($ids.Count -ne 1 -or $ids[0] -notmatch 'compared0=[1-9]\d* matches0=[1-9]\d* mismatches0=0' -or
                        $ids[0] -notmatch 'compared1=[1-9]\d* matches1=[1-9]\d* mismatches1=0' -or
                        $ids[0] -notmatch 'distinct0=[1-9]\d*' -or $ids[0] -notmatch 'distinct1=[1-9]\d*' -or
                        $ids[0] -notmatch 'id_without_motion_stencil=0') {
                        throw "$($case.Name): object ID comparison failed at frame 120"
                    }
                    $depthSamples = @($trace | Where-Object { $_ -match '\bframe=120 event=readback label=depth\b' })
                    if ($depthSamples.Count -ne 1 -or
                        $depthSamples[0] -notmatch 'id0_pixels=[1-9]\d* id1_pixels=[1-9]\d* ambiguous_pixels=0\b') {
                        throw "$($case.Name): missing IDs or ambiguous stencil over full render region"
                    }
                }
            }
            if ($case.Name -eq 'objects-baseline' -or $case.Name -eq 'objects-temporal') {
                foreach ($label in @('motion', 'depth')) {
                    $samples = @($trace | Where-Object { $_ -match "\bframe=120 event=readback label=$label\b" })
                    if ($samples.Count -ne 1 -or $samples[0] -notmatch 'full_hash=([0-9A-F]{16})\b') {
                        throw "$($case.Name): missing frame 120 $label sample hash"
                    }
                    $hash = $Matches[1]
                    if ($case.Name -eq 'objects-baseline') { $baselineHashes[$label] = $hash }
                    elseif ($hash -ne $baselineHashes[$label]) {
                        throw "$($case.Name): frame 120 $label samples changed with object ID pass"
                    }
                }
            }
        }
        if ($case.Name -eq 'coplanar-overlap') {
            $depthSamples = @($trace | Where-Object { $_ -match '\bframe=120 event=readback label=depth\b' })
            if ($depthSamples.Count -ne 1 -or $depthSamples[0] -notmatch '\bambiguous_pixels=[1-9]\d*\b') {
                throw "$($case.Name): expected full-region overlap was not detected"
            }
            $ids = @($trace | Where-Object { $_ -match '\bframe=120 event=object_id_motion_check\b' })
            if ($ids.Count -ne 1 -or $ids[0] -notmatch '\bambiguous=[1-9]\d*\b') {
                throw "$($case.Name): expected sampled overlap was not rejected as ambiguous"
            }
            if (-not $OverlapOnly) {
                foreach ($label in @('motion', 'depth')) {
                    $samples = @($trace | Where-Object { $_ -match "\bframe=120 event=readback label=$label\b" })
                    if ($samples.Count -ne 1 -or $samples[0] -notmatch 'full_hash=([0-9A-F]{16})\b' -or
                        $Matches[1] -ne $baselineHashes[$label]) {
                        throw "$($case.Name): full-region $label hash differs from baseline"
                    }
                }
            }
        }
        if ($case.Name -eq 'stencil-reset') {
            if ($checks.Count -lt 3 -or @($checks | Where-Object {
                $_ -notmatch 'compared=256 stencil_set=0 stencil_unset=256' -or $_ -notmatch '\bmismatches=0\b'
            }).Count) { throw "$($case.Name): stencil-unset reprojection validation failed" }
            foreach ($frame in @(120, 121, 240, 241)) {
                $reset = if ($frame -eq 120 -or $frame -eq 240) { 1 } else { 0 }
                $previous = 1 - $reset
                if (-not @($trace | Where-Object { $_ -match "frame=$frame event=frame .*previous_valid=$previous .*diagnostic_reset=$reset\b" }).Count -or
                    -not @($trace | Where-Object { $_ -match "frame=$frame event=ngx_dx11_evaluate_inputs .*reset=$reset\b" }).Count) {
                    throw "$($case.Name): missing expected reset or recovery at frame $frame"
                }
            }
            if (-not @($trace | Where-Object { $_ -match 'frame=240 event=frame .*diagnostic_cut=1' }).Count) {
                throw "$($case.Name): camera-cut evidence missing"
            }
        }
        $ngx = @($trace | Where-Object { $_ -match 'event=ngx_dx11_evaluate_result' })
        if (@($ngx | Where-Object { $_ -notmatch 'result=0x00000001' }).Count) {
            throw "$($case.Name): sampled NGX evaluation failed"
        }
        $taa = @($trace | Where-Object { $_ -match 'event=taa_bindings' })
        if (@($taa | Where-Object { $_ -notmatch 'identity_valid=1' }).Count) {
            throw "$($case.Name): invalid TAA bindings"
        }
        if (($case.Name -eq 'objects-dlss-camera' -or $case.Name -eq 'stencil-reset') -and $ngx.Count -eq 0) {
            throw "$($case.Name): no sampled NGX evaluations"
        }
        if (($case.Name -eq 'static-temporal' -or $case.Name -eq 'objects-temporal') -and $taa.Count -eq 0) {
            throw "$($case.Name): no sampled TAA bindings"
        }
        if ($case.Name -eq 'mode-transitions') {
            foreach ($mode in @('DLSS', 'TemporalAA', 'None')) {
                if (-not @($trace | Where-Object { $_ -match "event=frame mode=$mode\b" }).Count) {
                    throw "$($case.Name): missing mode $mode"
                }
            }
            if ($ngx.Count -eq 0 -or $taa.Count -eq 0) { throw "$($case.Name): missing temporal evaluations" }
        }
        if ($case.Name -eq 'frame-correlation') {
            $inputLines = @($trace | Where-Object { $_ -match '\bframe=120 event=ngx_dx11_evaluate_inputs\b' })
            if ($inputLines.Count -ne 1) { throw "$($case.Name): missing unique frame 120 DLSS inputs" }
            $inputLine = $inputLines[0]
            $qpcMatch = [regex]::Match($inputLine, '\bqpc=(\d+)')
            $motionMatch = [regex]::Match($inputLine, '\bmotion=([0-9A-Fa-f]+)')
            $depthMatch = [regex]::Match($inputLine, '\bdepth=([0-9A-Fa-f]+)')
            if (-not $qpcMatch.Success -or -not $motionMatch.Success -or -not $depthMatch.Success) {
                throw "$($case.Name): missing QPC or resource identities"
            }
        }
            if ($case.Name -eq 'shader-bindings') {
                $marker = @($trace | Where-Object { $_ -match '\bframe=120 event=gpu_sync_marker scope=taa_after_dispatch\b' })
                $completed = @($trace | Where-Object { $_ -match '\bframe=120 event=gpu_sync_complete scope=taa_after_dispatch\b' })
                if ($marker.Count -ne 1 -or $completed.Count -ne 1) {
                    throw "$($case.Name): GPU completion marker missing"
                }
                $inputs = @($trace | Where-Object { $_ -match '\bframe=120 event=taa_inputs\b' })
                $bindings = @($taa | Where-Object { $_ -match '\bframe=120 event=taa_bindings\b.*identity_valid=1' })
                if ($inputs.Count -ne 1 -or $bindings.Count -ne 1) {
                    throw "$($case.Name): missing frame 120 TAA inputs or binding verification"
                }
                $startMatch = [regex]::Match($inputs[0], '\bqpc=(\d+)')
                $endMatch = [regex]::Match($bindings[0], '\bqpc=(\d+)')
                if (-not $startMatch.Success -or -not $endMatch.Success) { throw "$($case.Name): missing QPC" }
                $startQpc = [long]$startMatch.Groups[1].Value
                $endQpc = [long]$endMatch.Groups[1].Value
                $markerQpc = [regex]::Match($marker[0], '\bqpc=(\d+)')
                $completeQpc = [regex]::Match($completed[0], '\bqpc=(\d+)')
                if (-not $markerQpc.Success -or -not $completeQpc.Success -or
                    $startQpc -ge $endQpc -or $endQpc -ge [long]$markerQpc.Groups[1].Value -or
                    [long]$markerQpc.Groups[1].Value -gt [long]$completeQpc.Groups[1].Value) {
                    throw "$($case.Name): GPU marker ordering is invalid"
                }
                $expected = @{}
                foreach ($entry in @(@('2:1', 'cs_t1_motion'), @('2:2', 'cs_t2_feedback_input'), @('3:1', 'cs_u1_feedback_output'))) {
                    $match = [regex]::Match($bindings[0], "\b$($entry[1])=([0-9A-Fa-f]+)")
                    if (-not $match.Success -or $match.Groups[1].Value -match '^0+$') {
                        throw "$($case.Name): missing resource $($entry[1])"
                    }
                    $expected[$entry[0]] = $match.Groups[1].Value.TrimStart('0').ToUpperInvariant()
                }
                $relevant = @{}
                $found = @{}
                $shaderHeaders = 0
                $shaderViews = 0
                $shaderSlots = 0
                $shaderCallbacks = $null
                $auxHeaders = 0
                $auxSlots = 0
                $auxViews = 0
                $auxCallbacks = $null
                $auxTypes = @{}
                $cleanShutdown = $false
                foreach ($line in [IO.File]::ReadLines($addonPath)) {
                    if ($line.Contains('event=logger_shutdown')) {
                        if ($line -match 'event=logger_shutdown dropped=0 file_write_failed=0 .*truncated=0') {
                            $cleanShutdown = $true
                        }
                        continue
                    }
                    if (-not $line.Contains('event=shader_') -and -not $line.Contains('event=addon_detach')) {
                        continue
                    }
                    if ($line -match 'event=(shader_bind|shader_view|shader_aux_bind) error=') { throw "$($case.Name): $line" }
                    if ($line -match 'event=shader_bind sequence=\d+ .* count=(\d+) qpc_valid=') {
                        ++$shaderHeaders
                        $shaderSlots += [long]$Matches[1]
                    }
                    elseif ($line -match 'event=shader_view sequence=\d+ index=') { ++$shaderViews }
                    elseif ($line -match 'event=addon_detach .*shader_bind_callbacks=(\d+)') { $shaderCallbacks = [long]$Matches[1] }
                    if ($line -match 'event=shader_aux_bind sequence=\d+ .*type=(\d+) count=(\d+) qpc_valid=') {
                        ++$auxHeaders
                        $auxTypes[$Matches[1]] = $true
                        $auxSlots += [long]$Matches[2]
                    }
                    elseif ($line -match 'event=shader_aux_slot sequence=\d+ index=') { ++$auxViews }
                    elseif ($line -match 'event=addon_detach .*shader_aux_callbacks=(\d+)') { $auxCallbacks = [long]$Matches[1] }
                    if ($line -match 'event=shader_bind sequence=(\d+) command_list=([0-9A-Fa-f]+) stages=32 .*type=(\d+) .*qpc_valid=1 qpc=(\d+)') {
                        $qpc = [long]$Matches[4]
                        if ($qpc -gt $startQpc -and $qpc -lt $endQpc) {
                            $relevant[$Matches[1]] = @{ Command = $Matches[2]; Type = $Matches[3] }
                        }
                    }
                    elseif ($line -match 'event=shader_view sequence=(\d+) index=\d+ binding=(\d+) view=0x[0-9A-Fa-f]+ resource=0x([0-9A-Fa-f]+)') {
                        $seq = $Matches[1]
                        if ($relevant.ContainsKey($seq)) {
                            $key = "$($relevant[$seq].Type):$($Matches[2])"
                            if ($expected.ContainsKey($key) -and $Matches[3].TrimStart('0').ToUpperInvariant() -eq $expected[$key]) {
                                $found[$key] = $relevant[$seq].Command
                            }
                        }
                    }
                }
                if ($found.Count -ne 3 -or $found['2:1'] -ne $found['2:2'] -or $found['2:1'] -ne $found['3:1']) {
                    throw "$($case.Name): TAA slots did not correlate within the same command list and QPC interval"
                }
                if ($null -eq $shaderCallbacks -or $shaderCallbacks -eq 0 -or
                    $shaderHeaders -ne $shaderCallbacks -or $shaderSlots -ne $shaderViews) {
                    throw "$($case.Name): shader callback or slot totals do not match"
                }
                if ($null -eq $auxCallbacks -or $auxCallbacks -eq 0 -or $auxHeaders -ne $auxCallbacks -or
                    $auxSlots -ne $auxViews -or -not $auxTypes.ContainsKey('0') -or -not $auxTypes.ContainsKey('6')) {
                    throw "$($case.Name): sampler or constant-buffer totals do not match"
                }
                if (-not $cleanShutdown) {
                    throw "$($case.Name): missing clean logger shutdown"
                }
                Write-Output "SHADER_BINDINGS callbacks=$shaderCallbacks slots=$shaderSlots"
                Write-Output "SHADER_AUX_BINDINGS callbacks=$auxCallbacks slots=$auxSlots types=sampler,constant_buffer"
                Write-Output "SHADER_CORRELATION frame=120 CS_t1=motion CS_t2=feedback_input CS_u1=feedback_output"
            }
        if ($case.Name -eq 'frame-correlation') {
            $evaluationQpc = [long]$qpcMatch.Groups[1].Value
            if ($evaluationQpc -le 0 -or -not @($ngx | Where-Object {
                $_ -match '\bframe=120 event=ngx_dx11_evaluate_result result=0x00000001\b'
            }).Count) {
                throw "$($case.Name): frame 120 has no valid QPC or successful DLSS result"
            }
            $motion = $motionMatch.Groups[1].Value.TrimStart('0')
            $depth = $depthMatch.Groups[1].Value.TrimStart('0')
            if (-not $motion -or -not $depth) { throw "$($case.Name): null temporal resource" }
            $markers = @($addon | Where-Object { $_ -match 'event=diagnostic_present .*qpc_valid=1' } |
                ForEach-Object {
                    $present = [regex]::Match($_, '\bpresent_counter=(\d+)')
                    $qpc = [regex]::Match($_, '\bqpc=(\d+)')
                    if ($present.Success -and $qpc.Success) {
                        [pscustomobject]@{ Present = [long]$present.Groups[1].Value; Qpc = [long]$qpc.Groups[1].Value }
                    }
                })
            $before = @($markers | Where-Object { $_.Qpc -lt $evaluationQpc } | Sort-Object Qpc | Select-Object -Last 1)
            $after = @($markers | Where-Object { $_.Qpc -gt $evaluationQpc } | Sort-Object Qpc | Select-Object -First 1)
            if ($before.Count -ne 1 -or $after.Count -ne 1 -or $after[0].Present -ne $before[0].Present + 1) {
                throw "$($case.Name): frame 120 is outside a consecutive detailed present interval"
            }
            $motionLines = @($addon | Where-Object {
                $_ -match "event=bound_view .*present_counter=$($before[0].Present)\b.*role=color index=3 .*resource=0x0*$motion\b"
            })
            $found = $false
            foreach ($motionLine in $motionLines) {
                $sequence = [regex]::Match($motionLine, '\bsequence=(\d+)').Groups[1].Value
                $depthLines = @($addon | Where-Object {
                    $_ -match "event=bound_view sequence=$sequence\b.*present_counter=$($before[0].Present)\b.*role=depth .*resource=0x0*$depth\b"
                })
                $times = @($addon | Where-Object {
                    $_ -match "event=diagnostic_bind_time sequence=$sequence\b.*present_counter=$($before[0].Present)\b.*qpc_valid=1"
                })
                if ($depthLines.Count -eq 0 -or $times.Count -ne 1) { continue }
                $bindQpcMatch = [regex]::Match($times[0], '\bqpc=(\d+)')
                if (-not $bindQpcMatch.Success) { continue }
                $bindQpc = [long]$bindQpcMatch.Groups[1].Value
                if ($before[0].Qpc -lt $bindQpc -and $bindQpc -lt $evaluationQpc -and $evaluationQpc -lt $after[0].Qpc) {
                    $found = $true
                    Write-Output "CORRELATION frame=120 present=$($before[0].Present)->$($after[0].Present) sequence=$sequence"
                    break
                }
            }
            if (-not $found) { throw "$($case.Name): no matching timed motion/depth binding in the present interval" }
        }
        if ($case.Name -eq 'all-target-bindings') {
            if (-not @($addon | Where-Object { $_ -match 'event=target_bind_capture mode=all scope=dx11_render_targets_and_depth' }).Count) {
                throw "$($case.Name): exhaustive target binding capture not active"
            }
            $seen = @{}
            $slots = @{}
            $times = @{}
            $expectedViews = @{}
            $observedViews = @{}
            $totalTargets = 0
            $targetLines = 0
            $callbackCount = $null
            foreach ($line in $addon) {
                if ($line -match 'event=(bind_targets|bound_view) error=') {
                    throw "$($case.Name): binding inspection error: $line"
                }
                if ($line -match 'event=bind_targets sequence=(\d+) .*target_count=(\d+) depth_view=0x([0-9A-Fa-f]+) .*detail=1') {
                    $seq = [long]$Matches[1]
                    if ($seen.ContainsKey($seq)) { throw "$($case.Name): duplicate bind sequence $seq" }
                    $seen[$seq] = [long]$Matches[2]
                    $totalTargets += [long]$Matches[2]
                    if ($Matches[3] -notmatch '^0+$') { $expectedViews["${seq}:depth:0"] = $Matches[3].TrimStart('0').ToUpperInvariant() }
                }
                elseif ($line -match 'event=bind_target sequence=(\d+) index=(\d+) view=0x([0-9A-Fa-f]+)') {
                    $key = "$($Matches[1]):$($Matches[2])"
                    if ($slots.ContainsKey($key)) { throw "$($case.Name): duplicate target slot $key" }
                    $slots[$key] = $true
                    if ($Matches[3] -notmatch '^0+$') {
                        $expectedViews["$($Matches[1]):color:$($Matches[2])"] = $Matches[3].TrimStart('0').ToUpperInvariant()
                    }
                    ++$targetLines
                }
                elseif ($line -match 'event=bound_view sequence=(\d+) .*role=(color|depth) index=(\d+) view=0x([0-9A-Fa-f]+)') {
                    $key = "$($Matches[1]):$($Matches[2]):$($Matches[3])"
                    if ($observedViews.ContainsKey($key)) { throw "$($case.Name): duplicate bound view $key" }
                    $observedViews[$key] = $Matches[4].TrimStart('0').ToUpperInvariant()
                }
                elseif ($line -match 'event=diagnostic_bind_time sequence=(\d+) .*qpc_valid=1 qpc=\d+') {
                    $seq = [long]$Matches[1]
                    if ($times.ContainsKey($seq)) { throw "$($case.Name): duplicate binding timestamp $seq" }
                    $times[$seq] = $true
                }
                elseif ($line -match 'event=addon_detach .*target_bind_callbacks=(\d+)') { $callbackCount = [long]$Matches[1] }
            }
            if ($null -eq $callbackCount -or $callbackCount -eq 0 -or $seen.Count -ne $callbackCount -or
                $totalTargets -ne $targetLines -or $times.Count -ne $callbackCount -or
                $expectedViews.Count -ne $observedViews.Count) {
                throw "$($case.Name): callback or target counts do not match"
            }
            for ($seq = 1; $seq -le $callbackCount; ++$seq) {
                if (-not $seen.ContainsKey([long]$seq)) { throw "$($case.Name): missing bind sequence $seq" }
                for ($index = 0; $index -lt $seen[[long]$seq]; ++$index) {
                    if (-not $slots.ContainsKey("${seq}:$index")) { throw "$($case.Name): missing slot ${seq}:$index" }
                }
            }
            foreach ($key in $expectedViews.Keys) {
                if (-not $observedViews.ContainsKey($key) -or $observedViews[$key] -ne $expectedViews[$key]) {
                    throw "$($case.Name): missing or mismatched bound view $key"
                }
            }
            Write-Output "TARGET_BINDINGS callbacks=$callbackCount target_slots=$targetLines views=$($observedViews.Count) gaps=0"
        }
        Write-Output "PASS $($case.Name) pid=$($process.Id) checks=$($checks.Count) ngx=$($ngx.Count) taa=$($taa.Count)"
        }
        finally {
            $process.Refresh()
            if (-not $process.HasExited) {
                if (-not $process.CloseMainWindow() -or -not $process.WaitForExit(10000)) {
                    Write-Warning "$($case.Name): stopping test process PID $($process.Id) after failed graceful close"
                    Stop-Process -Id $process.Id -ErrorAction Stop
                    $process.WaitForExit()
                }
            }
        }
    }
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
}
