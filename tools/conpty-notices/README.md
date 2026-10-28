# ConPTY redistribution notices

`LICENSE` and `NOTICE.md` are byte-for-byte copies from the Microsoft Terminal
source commit pinned by `../conpty-bootstrap.lock.json`; nested `.gitattributes`
preserves their upstream CRLF bytes. The complete upstream notice is retained
conservatively, including components beyond this minimal runtime pair. The NuGet
archive itself contains neither file; it declares MIT in its nuspec. The helper
ships these texts, the original nuspec and a complete provenance manifest beside
the verified DLL and host. It does not relicense StormTTY code.

Pin updates require explicit review of the archive, upstream provenance/notices,
full NuGet repository-signature verification and native acceptance results.
Ordinary developer builds need no .NET CLI: they require exact pinned archive and
payload hashes plus built-in Windows Microsoft Authenticode verification. These
are distinct checks; the latter does not claim to verify the NuGet repository
signature again. Trust failure is fatal, never an offline bypass.

This is native AMD64 Windows preview packaging only. Availability does not change
the System default; compact-alt resize compatibility remains unresolved.
