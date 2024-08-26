// |reftest| skip-if(this.getBuildConfiguration("pbl"))
// skip test if PBL enabled, because we do not get a full
// decompilation with variable names in the TypeError.

// Any copyright is dedicated to the Public Domain.
// http://creativecommons.org/licenses/publicdomain/

var err;
try {
    {let i=1}
    {let j=1; [][j][2]}
} catch (e) {
    err = e;
}
assertEq(err instanceof TypeError, true);
assertEq(err.message.endsWith("[][j] is undefined"), true);

reportCompare(0, 0, 'ok');
