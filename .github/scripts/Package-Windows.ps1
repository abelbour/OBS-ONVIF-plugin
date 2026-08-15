[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    # The zip targets the Windows SYSTEM plugin layout — the layout OBS
    # actually scans (obs-plugins\64bit + data\obs-plugins\<name>). Extract
    # into C:\Program Files\obs-studio\ (or the portable root) and the folders
    # merge: DLL under obs-plugins\64bit\, locale under
    # data\obs-plugins\obs-onvif\locale\.
    $Staging = "${ProjectRoot}/release/staging-${Target}"
    Remove-Item -Recurse -Force $Staging -ErrorAction SilentlyContinue

    $BinSrc = "${ProjectRoot}/release/${Configuration}/obs-onvif/bin/64bit"
    New-Item -ItemType Directory -Force -Path "${Staging}/obs-plugins/64bit" | Out-Null
    Copy-Item -Path "${BinSrc}/obs-onvif.dll" -Destination "${Staging}/obs-plugins/64bit/" -Force
    if (Test-Path "${BinSrc}/obs-onvif.pdb") {
        Copy-Item -Path "${BinSrc}/obs-onvif.pdb" -Destination "${Staging}/obs-plugins/64bit/" -Force
    }

    New-Item -ItemType Directory -Force -Path "${Staging}/data/obs-plugins/obs-onvif" | Out-Null
    Copy-Item -Recurse -Force "${ProjectRoot}/release/${Configuration}/obs-onvif/data/*" `
        -Destination "${Staging}/data/obs-plugins/obs-onvif/"

    # Public ABI header + docs, bundled under obs-onvif/ (not installed).
    New-Item -ItemType Directory -Force -Path "${Staging}/obs-onvif/docs" | Out-Null
    if (Test-Path "${ProjectRoot}/release/${Configuration}/obs-onvif/obs-onvif.h") {
        Copy-Item -Path "${ProjectRoot}/release/${Configuration}/obs-onvif/obs-onvif.h" `
            -Destination "${Staging}/obs-onvif/" -Force
    }
    foreach ( $Doc in @( 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md' ) ) {
        Copy-Item -Path "${ProjectRoot}/${Doc}" -Destination "${Staging}/obs-onvif/" -Force
    }
    Copy-Item -Path "${ProjectRoot}/docs/USER_GUIDE.md" -Destination "${Staging}/obs-onvif/docs/" -Force

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path $Staging)
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Remove-Item -Recurse -Force $Staging -ErrorAction SilentlyContinue
    Log-Group
}

Package
