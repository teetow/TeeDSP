# TeeDSP APO driver package

This folder contains the supported deployment shape for the low-latency APO:

- `TeeDspApoComponent.inf` installs the trusted APO software component from the Driver Store.
- `TeeDspRealtekExtension.inf` is the controlled test extension. It follows the installed Realtek UAD package's `InterfaceSetting` pattern and binds TeeDSP to the non-critical analogue output's `SingleLineOutTopo` interface.
- `New-TeeDspApoDriverPackage.ps1` stages the DLL and both INFs, runs Inf2Cat, signs the DLL before catalog generation, and signs the resulting catalog.

The component and extension must be catalog-signed as one driver package. Test deployment requires a trusted test certificate and test-signing mode; production deployment requires a production-signed driver package. Do not use the legacy `apo/install/Install.ps1` script for this deployment path. The Realtek extension is deliberately a test target; it does not bind to AirPods.

Stage an unsigned package for validation:

```powershell
.\apo\driver\New-TeeDspApoDriverPackage.ps1 `
  -DllPath .\out\build\vs2022\apo\Release\TeeDspApo.dll `
  -OutputDir .\out\driver-package
```

Pass `-CertThumbprint <thumbprint>` to sign both the DLL and catalog. Package installation is deliberately separate from staging: it requires a test/production signing decision and changes the audio driver stack.
