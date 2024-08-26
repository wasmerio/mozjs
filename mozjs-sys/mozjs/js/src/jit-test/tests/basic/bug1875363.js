// |jit-test| --fuzzing-safe; --baseline-eager; --arm-hwcap=vfp; skip-if: getBuildConfiguration('pbl')
// (skip on PBL because there is no native code on the JitScript to disassemble.)
function f() {};
f();
f();
f();
try {
    print(disnative(f));
} catch (e) {}
