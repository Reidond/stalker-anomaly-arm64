// --pre-js of the Node test executables (cmake/web/AnomalyWebFlags.cmake): hands the process command line to the C++ side.
// The engine reads it through GetCommandLine() (Core.Params: -mt_cdb, -wf, -nocache ...); a wasm module has no argv outside main().
// GetCommandLine() on the web build returns $XR_COMMAND_LINE; the browser shell sets the same thing with xr_set_command_line().
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  // ENV exists only in a module that links getenv() or environ (ode_smoke does not)
  if (typeof ENV === 'undefined' || typeof process === 'undefined' || !process.argv) return;
  var quote = function (a) { return a.indexOf(' ') >= 0 ? '"' + a + '"' : a; };
  var args = process.argv.slice(2).map(quote);
  ENV['XR_COMMAND_LINE'] = [quote(Module['thisProgram'] || 'anomaly')].concat(args).join(' ');
});
