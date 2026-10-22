// Starter SynthDefs following midid's convention:
//   freq, note, vel (0..1), amp (vel squared), bend (semitones),
//   gate (0 releases; the def must free itself), buf/rate for samples,
//   plus whatever controls midid.conf maps CCs to (here: cutoff).
//
// Load tools/pi.scd first, then run this block to audition and upload.

(
~piPut.(SynthDef(\epiano, { |out=0, freq=440, vel=0.5, amp=0.25, gate=1, bend=0, cutoff=6000|
	var f = freq * bend.midiratio;
	var env = EnvGen.kr(Env.adsr(0.002, 1.5, 0.3, 0.5), gate, doneAction: Done.freeSelf);
	var tine = EnvGen.kr(Env.perc(0.001, 0.6)) * vel * 1.5;
	var sig = SinOsc.ar(f, SinOsc.ar(f * 14, 0, tine)) + (SinOsc.ar(f * 2) * 0.1 * vel);
	sig = LPF.ar(sig, cutoff.clip(200, 18000));
	Out.ar(out, Pan2.ar(sig * env * amp, 0));
}));

// Stereo sample player. midid picks the nearest "sample" line from
// midid.conf and passes buf and rate for the pitch shift.
~piPut.(SynthDef(\sampler, { |out=0, buf=0, rate=1, amp=0.25, gate=1, bend=0, rel=0.3|
	var env = EnvGen.kr(Env.asr(0.001, 1, rel), gate, doneAction: Done.freeSelf);
	var sig = PlayBuf.ar(2, buf, BufRateScale.kr(buf) * rate * bend.midiratio, doneAction: Done.freeSelf);
	Out.ar(out, sig * env * amp * 2);
}));

// Velocity-layered sample player (see the piano "sample" lines in
// midid.conf). Each layer already has its own loudness and tone, so the
// gain only covers the distance from the layer's velocity (vel/svel).
// The release is a damper falling on the strings.
~piPut.(SynthDef(\piano, { |out=0, buf=0, rate=1, vel=0.5, svel=0.5, gate=1, bend=0, rel=0.35|
	var gain = (vel / svel.max(0.01)).clip(0.5, 2);
	var env = EnvGen.kr(Env.asr(0.001, 1, rel, -4), gate, doneAction: Done.freeSelf);
	var sig = PlayBuf.ar(2, buf, BufRateScale.kr(buf) * rate * bend.midiratio, doneAction: Done.freeSelf);
	Out.ar(out, sig * env * gain * 0.4);
}));

~piPut.(SynthDef(\pad, { |out=0, freq=440, amp=0.25, gate=1, bend=0, cutoff=2000|
	var f = freq * bend.midiratio * [1, 1.004, 0.996];
	var env = EnvGen.kr(Env.adsr(0.4, 0.5, 0.7, 1.5), gate, doneAction: Done.freeSelf);
	var sig = Splay.ar(Saw.ar(f));
	sig = RLPF.ar(sig, cutoff.clip(100, 12000), 0.4);
	Out.ar(out, sig * env * amp * 0.4);
}));
)
