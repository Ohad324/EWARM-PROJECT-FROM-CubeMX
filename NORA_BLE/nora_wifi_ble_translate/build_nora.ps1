# Build NORA via a fresh Windows process (no Cygwin/MSYS env variables)
$IDF_PATH   = 'C:\Espressif\frameworks\esp-idf-v5.5.3'
$PYTHON     = 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$PROJECT    = 'C:\TouchGFXProjects\MyApplication\NORA_BLE\nora_wifi_ble_translate'

# Build a clean environment
$cleanEnv = @{
    'IDF_PATH'          = $IDF_PATH
    'IDF_TOOLS_PATH'    = 'C:\Espressif'
    'SYSTEMROOT'        = $env:SYSTEMROOT
    'TEMP'              = $env:TEMP
    'TMP'               = $env:TMP
    'USERPROFILE'       = $env:USERPROFILE
    'APPDATA'           = $env:APPDATA
    'LOCALAPPDATA'      = $env:LOCALAPPDATA
    'COMSPEC'           = $env:COMSPEC
    'PYTHONNOUSERSITE'  = 'True'
    'OS'                        = 'Windows_NT'
    'PROCESSOR_ARCHITECTURE'    = $env:PROCESSOR_ARCHITECTURE
    'PROCESSOR_IDENTIFIER'      = $env:PROCESSOR_IDENTIFIER
    'NUMBER_OF_PROCESSORS'      = $env:NUMBER_OF_PROCESSORS
    'WINDIR'                    = $env:WINDIR
    'PATH'              = @(
        'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts'
        'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin'
        'C:\Espressif\tools\cmake\3.30.2\bin'
        'C:\Espressif\tools\ninja\1.12.1'
        'C:\Espressif\tools\idf-git\2.44.0\cmd'
        'C:\Espressif\frameworks\esp-idf-v5.5.3\tools'
        'C:\Espressif'
        "$env:SYSTEMROOT\System32"
        "$env:SYSTEMROOT"
    ) -join ';'
}

$outFile = "$PROJECT\build_out.txt"
$errFile = "$PROJECT\build_err.txt"

$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName               = $PYTHON
$psi.Arguments              = "`"$IDF_PATH\tools\idf.py`" -C `"$PROJECT`" build"
$psi.UseShellExecute        = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true

$psi.EnvironmentVariables.Clear()
foreach ($kv in $cleanEnv.GetEnumerator()) {
    $psi.EnvironmentVariables[$kv.Key] = $kv.Value
}

$proc = [System.Diagnostics.Process]::Start($psi)
$stdout = $proc.StandardOutput.ReadToEnd()
$stderr = $proc.StandardError.ReadToEnd()
$proc.WaitForExit()

Write-Host $stdout
if ($stderr) { Write-Host "[STDERR]"; Write-Host $stderr }
Write-Host "Build finished - exit code: $($proc.ExitCode)"
exit $proc.ExitCode
