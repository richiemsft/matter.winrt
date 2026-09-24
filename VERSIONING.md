# Versioning, compatibility, and servicing

`Matter.Windows.Controller` is the supported application boundary. Its public
binary contract consists of the WinRT metadata and these standard activation
exports:

- `DllCanUnloadNow`
- `DllGetActivationFactory`

Matter SDK C++ headers, classes, STL types, and internal symbols are not public
ABI.

## Package versions

The NuGet package uses semantic versioning:

- Preview versions may change API or ABI between releases. Applications must
  deploy the native DLL from the exact package used at build time.
- Stable patch releases contain compatible fixes and no intentional public API
  additions.
- Stable minor releases may add APIs while preserving all metadata from earlier
  releases in the same major version.
- Breaking metadata or ABI changes require a new major version.

Each package pins one Matter SDK commit. Updating that commit requires a package
version change plus x64 and ARM64 component builds, sample builds against the
produced package, WinMD compatibility validation, and native export validation.

## Compatibility gates

`tools/verify-abi.ps1` converts the current and baseline WinMD files to IDL with
the Windows SDK. Existing interface declarations must remain identical except
that deprecation metadata may be added. Existing enum and runtime-class
declarations may gain metadata but may not remove or change prior metadata. The
normalized public declarations in the x64 and ARM64 WinMD files must be
equivalent; non-contract metadata may differ.

The same gate compares each architecture's export table with
`abi/public-exports.txt`. Any missing or additional native export fails the
build.

The Windows workflow compares pull requests with the most recently adopted
release baseline. Updating that baseline is a compatibility-policy change and
must identify the required package-version impact in the pull request.

## Deprecation

A stable API must be marked deprecated for at least one minor release and six
months before it can be removed in a subsequent major release. Release notes
must identify the replacement and the earliest removal version. An immediate
security fix may shorten this period when retaining the API would expose users
to a material vulnerability.

## Servicing

Before the first stable release, packages are previews without a servicing
commitment. After a stable release:

- The latest minor release in each supported major receives security and
  correctness fixes.
- The preceding minor release receives critical security fixes for six months
  after its successor ships.
- Supported major versions and end-of-support dates are listed in release
  notes.
- Fixes that alter the public contract follow the semantic-versioning and
  deprecation rules above.
