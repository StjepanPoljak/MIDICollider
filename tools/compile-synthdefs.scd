// Writes the defs from tools/synthdefs.scd into data/synthdefs, so a fresh
// SD card image has something to play. Run with: make synthdefs
(
var root = thisProcess.nowExecutingPath.dirname.dirname;
~piPut = { |def| def.writeDefFile(root +/+ "data/synthdefs"); def };
(root +/+ "tools/synthdefs.scd").load;
0.exit;
)
