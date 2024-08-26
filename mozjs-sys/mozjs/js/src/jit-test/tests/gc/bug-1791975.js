// |jit-test| skip-if: !('oomAtAllocation' in this) || getBuildConfiguration('pbl')
//
// We skip this test under PBL-debug builds because it seems to have a
// nondeterministic OOM failure that we can't reproduce locally. Other tests
// provide coverage of OOM handling generally so we don't lose too much
// (hopefully!) by skipping this one.

gczeal(10, 10);
try {
  throw 0;
} catch {
  for (let i = 1; i < 20 ; i++) {
    oomAtAllocation(i);
    try {
      newGlobal();
    } catch {}
  }
}
