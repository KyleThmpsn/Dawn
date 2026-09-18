`omega_loading_roster.bin` contains the 16-group loading roster reconstructed
from the installed format-65 build cache on 2026-09-18 at region 120. The cache
SHA-256 is `5B981CCE4F353498624BEA52064374A03C673B42CA0A8AEAAE170047BE70140F`.

The production `fill_roster` produced nine groups after the Forest amendment.
The reveal, boss, Crown, and four `kCombatGroups` admissions produced sixteen,
with three top-level groups. This reproduces the logged nine-to-twelve progression
before the four combat admissions. The unchanged archive encoder rejected the
complete roster; changing only its group count to fifteen made encoding succeed.
The regression requires all sixteen groups and retains their native bubble scopes.

The compact fixture contains no account or player data. All scalars are little
endian: u32 group count, top-level count, player registry key; then each group's
u32 key, u16 slot count, and `(u8 type, u8 flags, u16 index)` slots; then u32 bubble
count and each bubble's u32 index, u32 key count, and u32 keys. Its SHA-256 is
`E5DB5928083074C147E4BDA5EAC2D37CCB49A427F5DA8D2F4285D74D1A08D8DF`.
