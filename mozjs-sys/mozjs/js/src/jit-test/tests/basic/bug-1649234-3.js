// |jit-test| exitstatus: 3; skip-if: getBuildConfiguration()['pbl']
//
// (skip if PBL is enabled: it doesn't support the debugger)

let debuggerRealm = newGlobal({newCompartment: true});
debuggerRealm.debuggee = this;
debuggerRealm.eval(`
  let dbg = new Debugger(debuggee);
  dbg.onDebuggerStatement = (frame) => null;  // terminate the debuggee
`);

Atomics.add(new Int32Array(1), 0, {
  valueOf() {
    debugger;
  }
});
