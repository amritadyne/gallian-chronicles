# VC2 Windows resources

`VC2Native.rc` embeds `vc2native.ico` as application icon resource ID 1 for
the native Windows executable.  The icon preserves the alpha-enabled 128×128
VC2 artwork supplied for the launcher; it was packaged as a standard Windows
ICO because the supplied file itself was PNG data despite its `.ico` label.

The resource is compiled separately by `profiles/vc2/CMakeLists.txt`.  This
avoids a MinGW `windres` preprocessing issue with spaces in the path, then
links the resulting COFF resource object into every
`VC2Native` build configuration.

Verified on 2026-11-08: both the release and trace executables expose Windows
`RT_GROUP_ICON` resource ID 1.
