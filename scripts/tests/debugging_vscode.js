const vscode = require('vscode');
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {spawn} = require('child_process');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
exports.run = async function () {
 const context = JSON.parse(fs.readFileSync(process.env.MAPPLE_DEBUG_TEST_CONTEXT,'utf8'));
 const root = context.root;
 const results = {checks: []};
 const messages = [];
 const transcript = process.env.MAPPLE_DEBUG_TEST_RESULTS+'.dap.jsonl';
 fs.writeFileSync(transcript,'');
 const ended = new Set();
 const taskEvents=[];
 const taskStarts=vscode.tasks.onDidStartTaskProcess(e=>taskEvents.push({name:e.execution.task.name,pid:e.processId,event:'start'}));
 const taskEnds=vscode.tasks.onDidEndTaskProcess(e=>taskEvents.push({name:e.execution.task.name,event:'end',code:e.exitCode}));
 const tracker = vscode.debug.registerDebugAdapterTrackerFactory('mapple-kernel', {
  createDebugAdapterTracker(session) {return {
   onWillReceiveMessage(message) {fs.appendFileSync(transcript,JSON.stringify({direction:'to-adapter',session:session.id,...message})+'\n');},
   onDidSendMessage(message) {messages.push({session:session.id,...message});fs.appendFileSync(transcript,JSON.stringify({direction:'from-adapter',session:session.id,...message})+'\n');}
  };}
 });
 const termination = vscode.debug.onDidTerminateDebugSession(session => ended.add(session.id));
 let active;
 let child;
 async function until(predicate, label, timeout = 60000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {const value=predicate();if(value) return value;await delay(100);}
  throw new Error('Timed out: '+label);
 }
 function infoRequest() {
  child=spawn('go',['test','-tags=integration','-count=1','-run','^TestIntegration_Info$','./server/internal/moduleclient'],{
   cwd:path.join(root,'user'),env:{...process.env,GOTOOLCHAIN:context.toolchain,MAPPLE_TEST_TCP_HOST:context.machine.host,MAPPLE_TEST_TCP_PORT:String(context.machine.module_port)}
  });
  let output='';child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>output+=b);
  const done=new Promise((resolve,reject)=>{child.on('error',reject);child.on('exit',code=>code===0?resolve():reject(new Error(output)));});
  done.catch(()=>{});return done;
 }
 function record() {return JSON.parse(fs.readFileSync(path.join(context.lab,'runs',context.run,'record.json'),'utf8'));}
 async function restart(label) {
  const before=record();
  const from=messages.length;
  await vscode.commands.executeCommand('workbench.action.debug.restart');
  // VS Code may retain the session ID while replacing its debug adapter.
  await until(()=>record().debugger_pid!==before.debugger_pid&&record().status==='debugging','Restart '+label,90000);
  active=await until(()=>vscode.debug.activeDebugSession,'Restart active session '+label);
  await until(()=>messages.slice(from).find(m=>m.session===active.id&&m.event==='thread'&&m.body.reason==='started'),'Restart CPU threads '+label);
  const after=record();
  (results.restarts ||= []).push({label,before,after,tasks:[...taskEvents]});
  assert.strictEqual(after.boot_id,before.boot_id,'Restart must not reboot the VM');
  assert.notStrictEqual(after.load_receipt,before.load_receipt,'Restart must reload the module');
  assert.notStrictEqual(after.debugger_pid,before.debugger_pid,'Restart must use a fresh debugger');
  assert.strictEqual(after.status,'debugging');
  results.checks.push('Toolbar Restart '+label+' rebuilds/reloads and reconnects without rebooting');
 }
 try {
  await vscode.extensions.getExtension('mapple.navigation').activate();
  await vscode.workspace.getConfiguration('debug').update('onTaskErrors','abort',vscode.ConfigurationTarget.Workspace);
  const source = vscode.Uri.file(path.join(root,'kernel/src/control/commands/info/info.c'));
  const breakpoint = new vscode.SourceBreakpoint(new vscode.Location(source,new vscode.Position(20,0)));
  await vscode.commands.executeCommand('workbench.action.debug.start');
  active = await until(()=>vscode.debug.activeDebugSession,'active session');
  results.checks.push('Actual VS Code start and generated prelaunch task');
  await until(()=>messages.find(m=>m.session===active.id&&m.event==='thread'&&m.body.reason==='started'),'initial CPU threads');
  await delay(500);
  for(let cycle=0;cycle<3;cycle++) {
   const beforePause=messages.length;
   await vscode.commands.executeCommand('workbench.action.debug.pause');
   const pause=await until(()=>messages.slice(beforePause).find(m=>m.session===active.id&&m.event==='stopped'),'toolbar Pause '+cycle,15000);
   const pauseFrames=await active.customRequest('stackTrace',{threadId:pause.body.threadId,levels:1});
   assert(pauseFrames.stackFrames.length,'Pause must expose a real stopped frame');
   await vscode.commands.executeCommand('workbench.action.debug.continue');
   await infoRequest();
   results.checks.push('Actual toolbar Pause/inspect/Continue and live request, cycle '+cycle);
  }
  for(const command of ['continue','c']) {
   const start=messages.length;
   await vscode.commands.executeCommand('workbench.action.debug.pause');
   const pause=await until(()=>messages.slice(start).find(m=>m.session===active.id&&m.event==='stopped'),'Pause before console '+command);
   const frame=(await active.customRequest('stackTrace',{threadId:pause.body.threadId,levels:1})).stackFrames[0];
   const from=messages.length;
   await active.customRequest('evaluate',{expression:'-exec '+command,frameId:frame.id,context:'repl'});
   await until(()=>messages.slice(from).find(m=>m.session===active.id&&m.event==='continued'),'console Continue UI synchronization');
   await infoRequest();
   results.checks.push('Debug Console -exec '+command+' resumes and synchronizes the toolbar');
  }
  await restart('while running');
  await infoRequest();
  const beforeRestartPause=messages.length;
  await vscode.commands.executeCommand('workbench.action.debug.pause');
  await until(()=>messages.slice(beforeRestartPause).find(m=>m.session===active.id&&m.event==='stopped'),'Pause before Restart');
  await restart('while paused');
  await infoRequest();
  vscode.debug.addBreakpoints([breakpoint]);await delay(300);
  const childDone=infoRequest();
  const stopped=await until(()=>messages.find(m=>m.session===active.id&&m.event==='stopped'&&m.body.reason==='breakpoint'),'source breakpoint');
  const tid=stopped.body.threadId;
  let frame=(await active.customRequest('stackTrace',{threadId:tid,levels:1})).stackFrames[0];
  assert.strictEqual(frame.source.path,source.fsPath);assert.strictEqual(frame.line,21);
  results.checks.push('Red-dot source breakpoint maps to the correct Mapple source line');
  const watch=await active.customRequest('evaluate',{expression:'context->session->id',frameId:frame.id,context:'watch'});
  assert(watch.result);results.checks.push('Source-frame variable inspection');
  const instructions=await active.customRequest('disassemble',{memoryReference:frame.instructionPointerReference,instructionCount:4,resolveSymbols:true});
  assert.strictEqual(instructions.instructions.length,4);
  const memory=await active.customRequest('readMemory',{memoryReference:frame.instructionPointerReference,count:16});
  assert.strictEqual(Buffer.from(memory.data,'base64').length,16);
  results.checks.push('VS Code disassembly and memory reads');
  for(const helper of ['lx-ps','lx-dmesg']) {
   const value=await active.customRequest('evaluate',{expression:'-exec '+helper,frameId:frame.id,context:'repl'});
   assert(value.result&&value.result.includes(helper==='lx-ps'?'PID':'Linux version'));
   (results.helpers ||= {})[helper]=value.result;results.checks.push('Debug Console '+helper);
  }
  const beforeInto=messages.length;
  await vscode.commands.executeCommand('workbench.action.debug.stepInto');
  const into=await until(()=>messages.slice(beforeInto).find(m=>m.session===active.id&&m.event==='stopped'),'VS Code Step Into');
  const intoFrame=(await active.customRequest('stackTrace',{threadId:into.body.threadId,levels:1})).stackFrames[0];
  assert(intoFrame.name.includes('mapple_command_write_response_header'),JSON.stringify(intoFrame));
  results.checks.push('Actual VS Code Step Into reaches the called Mapple function');
  const beforeOut=messages.length;
  await vscode.commands.executeCommand('workbench.action.debug.stepOut');
  const out=await until(()=>messages.slice(beforeOut).find(m=>m.session===active.id&&m.event==='stopped'),'VS Code Step Out');
  const outFrame=(await active.customRequest('stackTrace',{threadId:out.body.threadId,levels:1})).stackFrames[0];
  assert.strictEqual(outFrame.source.path,source.fsPath);
  results.checks.push('Actual VS Code Step Out returns to the Mapple caller');
  await delay(200);
  const before=messages.length;
  await vscode.commands.executeCommand('workbench.action.debug.stepOver');
  await until(()=>messages.slice(before).find(m=>m.session===active.id&&m.event==='stopped'),'VS Code Step Over');
  frame=(await active.customRequest('stackTrace',{threadId:tid,levels:1})).stackFrames[0];assert.notStrictEqual(frame.line,21);
  results.checks.push('Actual VS Code Step Over command');
  vscode.debug.removeBreakpoints([breakpoint]);
  const linuxSource=vscode.Uri.file(path.join(context.kernel_source,'net/socket.c'));
  const linuxLines=fs.readFileSync(linuxSource.fsPath,'utf8').split('\n');
  const linuxStart=linuxLines.findIndex(line=>line.startsWith('int kernel_sendmsg('));
  const linuxLine=linuxLines.findIndex((line,i)=>i>=linuxStart&&line.includes('iov_iter_kvec('));
  const linuxBreakpoint=new vscode.SourceBreakpoint(new vscode.Location(linuxSource,new vscode.Position(linuxLine,0)));
  vscode.debug.addBreakpoints([linuxBreakpoint]);await delay(300);
  const beforeLinux=messages.length;
  await vscode.commands.executeCommand('workbench.action.debug.continue');
  const linuxStop=await until(()=>messages.slice(beforeLinux).find(m=>m.session===active.id&&m.event==='stopped'&&m.body.reason==='breakpoint'),'Linux source breakpoint');
  const linuxTid=linuxStop.body.threadId;
  const linuxFrame=(await active.customRequest('stackTrace',{threadId:linuxTid,levels:1})).stackFrames[0];
  assert.strictEqual(fs.realpathSync(linuxFrame.source.path),fs.realpathSync(linuxSource.fsPath));
  const linuxWatch=await active.customRequest('evaluate',{expression:'sock',frameId:linuxFrame.id,context:'watch'});
  assert(linuxWatch.result&&!linuxWatch.result.includes('optimized out'));
  results.checks.push('Actual VS Code Linux C source breakpoint and sock inspection');
  const beforeLinuxStep=messages.length;
  await vscode.commands.executeCommand('workbench.action.debug.stepOver');
  await until(()=>messages.slice(beforeLinuxStep).find(m=>m.session===active.id&&m.event==='stopped'),'Linux VS Code Step Over');
  const linuxStepped=(await active.customRequest('stackTrace',{threadId:linuxTid,levels:1})).stackFrames[0];
  assert.notStrictEqual(linuxStepped.line,linuxFrame.line);
  results.checks.push('Actual VS Code Linux C source Step Over command');
  vscode.debug.removeBreakpoints([linuxBreakpoint]);await delay(200);
  await vscode.commands.executeCommand('workbench.action.debug.continue');
  await childDone;results.checks.push('Actual VS Code Continue command and completed live request');
  vscode.debug.addBreakpoints([breakpoint]);await delay(300);
  const beforeRebind=messages.length;
  const interruptedRequest=infoRequest();
  await until(()=>messages.slice(beforeRebind).find(m=>m.session===active.id&&m.event==='stopped'&&m.body.reason==='breakpoint'),'breakpoint before toolbar Restart');
  await restart('at a source breakpoint');
  await interruptedRequest;
  const afterRebind=messages.length;
  const reboundRequest=infoRequest();
  const rebound=await until(()=>messages.slice(afterRebind).find(m=>m.session===active.id&&m.event==='stopped'&&m.body.reason==='breakpoint'),'preserved red dot after toolbar Restart');
  const reboundFrame=(await active.customRequest('stackTrace',{threadId:rebound.body.threadId,levels:1})).stackFrames[0];
  assert.strictEqual(reboundFrame.source.path,source.fsPath);assert.strictEqual(reboundFrame.line,21);
  results.checks.push('Toolbar Restart preserves and rebinds the existing red-dot breakpoint to the reloaded module');
  vscode.debug.removeBreakpoints([breakpoint]);await delay(200);
  await vscode.commands.executeCommand('workbench.action.debug.continue');await reboundRequest;
  const first=active;
  await vscode.commands.executeCommand('workbench.action.debug.stop');
  await until(()=>ended.has(first.id),'VS Code Stop');
  await until(()=>JSON.parse(fs.readFileSync(path.join(context.lab,'runs',context.run,'record.json'),'utf8')).status==='detached','post-debug cleanup',15000);
  results.run_after_stop=JSON.parse(fs.readFileSync(path.join(context.lab,'runs',context.run,'record.json'),'utf8')).status;
  results.checks.push('Actual VS Code Stop command');
  vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(source,new vscode.Position(20,0)))]);
  let restarted=false;
  try {
   await vscode.commands.executeCommand('workbench.action.debug.start');
   await until(()=>vscode.debug.activeDebugSession?.id!==first.id&&vscode.debug.activeDebugSession,'F5 after Stop');
   restarted=true;
  } catch(error) {results.restart_error=error.message;}
  results.restart_succeeded=restarted;
  results.result=restarted?'PASS':'FAIL';
  if(restarted) {
   active=await until(()=>vscode.debug.activeDebugSession?.id!==first.id&&vscode.debug.activeDebugSession,'restarted session');
   results.checks.push('Same workspace starts again after Stop');
   const secondRequest=infoRequest();
   await until(()=>messages.find(m=>m.session===active.id&&m.event==='stopped'&&m.body.reason==='breakpoint'),'breakpoint after restart');
   results.checks.push('Source breakpoint works again after restart');
   const second=active;await vscode.commands.executeCommand('workbench.action.debug.stop');await until(()=>ended.has(second.id),'second Stop');
   await until(()=>JSON.parse(fs.readFileSync(path.join(context.lab,'runs',context.run,'record.json'),'utf8')).status==='detached','second post-debug cleanup',15000);
   results.checks.push('Second Stop also completes post-debug cleanup');
   await secondRequest;
   results.checks.push('Stop at a breakpoint resumes the guest and completes the live request');
  }
 } catch(error) {results.result='FAIL';results.error=error.stack;}
 finally {
  if(vscode.debug.activeDebugSession) await Promise.race([vscode.debug.stopDebugging(vscode.debug.activeDebugSession),delay(10000)]);
  if(child&&child.exitCode===null) child.kill();
  vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
  tracker.dispose();termination.dispose();taskStarts.dispose();taskEnds.dispose();
  fs.writeFileSync(process.env.MAPPLE_DEBUG_TEST_RESULTS,JSON.stringify(results,null,2));
 }
 if(results.result!=='PASS') throw new Error(JSON.stringify(results));
};
