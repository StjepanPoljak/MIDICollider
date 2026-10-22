// Laptop-side helpers: audition a SynthDef locally, then push it to the Pi.
//
//   "~/Develop/MIDICollider/tools/pi.scd".standardizePath.load;   // or open and run this file
//   ~piPut.(SynthDef(\pad, { ... }));
//
// The def is added to the local server too, so you can play it right away.

(
~piTool = thisProcess.nowExecutingPath.dirname +/+ "pi";
~piDir = "~/.cache/midicollider/synthdefs".standardizePath;

~piPut = { |def|
	var file = ~piDir +/+ def.name ++ ".scsyndef";
	File.mkdir(~piDir);
	def.add;
	def.writeDefFile(~piDir);
	"% put %".format(~piTool.shellQuote, file.shellQuote).unixCmd;
	def
};

~piRestart = { "% restart".format(~piTool.shellQuote).unixCmd };
)
