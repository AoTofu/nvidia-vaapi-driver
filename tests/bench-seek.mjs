// Node >=22. Real headed Chrome, native CDP, private profile, no dependencies.
// Usage: node tests/bench-seek.mjs DRIVER_DIR VIDEO OUTPUT_JSON COUNT [LAYOUT]
// NVD_BENCH_MODE=paused|playing|arrows|burst; NVD_BENCH_REQUIRE_CORRECT=1
// enables failure exits for mismatches, software fallback or a wrong driver.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
const [driverDir, videoFile, output, countArg = '75', layout = 'packed'] = process.argv.slice(2);
if (!output) throw Error('Expected DRIVER_DIR VIDEO OUTPUT_JSON COUNT [LAYOUT]');
const count = Number(countArg);
if(!Number.isInteger(count)||count<1||count>3000)throw Error('COUNT must be an integer from 1 to 3000');
const mode = process.env.NVD_BENCH_MODE ?? 'paused';
if (!['paused', 'playing', 'arrows', 'burst'].includes(mode)) throw Error('Invalid NVD_BENCH_MODE');
const videoSha256 = createHash('sha256').update(fs.readFileSync(videoFile)).digest('hex');
const fixture = fs.existsSync(videoFile + '.json') ? JSON.parse(fs.readFileSync(videoFile + '.json', 'utf8')) : null;
if (fixture && fixture.sha256 !== videoSha256) throw Error('Fixture manifest hash mismatch');
const fps = fixture?.fps ?? 30;
const runDir = fs.mkdtempSync(path.join(os.tmpdir(), 'nvd-seek-'));
const profile = path.join(runDir, 'profile');
const mediaEvents = [];
const html = `<!doctype html><meta charset="utf-8"><title>NVIDIA seek benchmark</title>
<body style="background:#333;color:white"><h2>Dedicated seek benchmark</h2>
<video id="v" muted autoplay src="/video" style="width:960px"></video><canvas id="c" hidden></canvas>
<script>
window.runSeeks = async function(count) {
 const v = document.querySelector('#v'), c = document.querySelector('#c');
 const mode=${JSON.stringify(mode)},fps=${fps},fixture=${JSON.stringify(fixture)};
 if (v.readyState < 2) await new Promise((resolve,reject)=>{v.onloadeddata=resolve;v.onerror=()=>reject(Error('video load failed'))});
 await v.play();
 const playbackStart=performance.now(),qualityStart=v.getVideoPlaybackQuality();
 await new Promise(r=>setTimeout(r,3000));
 if(mode==='paused')v.pause();
 const qualityEnd=v.getVideoPlaybackQuality();
 const playback={elapsedMs:performance.now()-playbackStart,
   totalFrames:qualityEnd.totalVideoFrames-qualityStart.totalVideoFrames,
   droppedFrames:qualityEnd.droppedVideoFrames-qualityStart.droppedVideoFrames};
 c.width=v.videoWidth;c.height=v.videoHeight;
 if(fixture&&(c.width!==fixture.width||c.height!==fixture.height))throw Error('Fixture dimensions mismatch');
 const markerRows=fixture?.markerRows??[32,c.height-32];
 const frameCount=fixture?.frames??Math.round(v.duration*fps);
 const ctx=c.getContext('2d',{willReadFrequently:true});
 const rows=[];
 let seekState,keyEvents=0;
 window.addEventListener('keydown',event=>{
   if(!seekState?.pendingKeys||!['ArrowRight','ArrowLeft'].includes(event.key))return;
   event.preventDefault();
   keyEvents++;
   const span=v.duration-0.5;
   // Wrap at the fixture ends so repeated +5/-5s never becomes a no-op.
   const target=((v.currentTime-0.25+(event.key==='ArrowRight'?5:-5))%span+span)%span+0.25;
   seekState.target=target;seekState.pendingKeys--;v.currentTime=target;
 });
 for(let i=0;i<count+10;i++) {
   seekState={target:((i*97)%(frameCount-30)+15)/fps,pendingKeys:0};
   const keys=mode==='arrows'?[i%3===2?'ArrowLeft':'ArrowRight']:
     mode==='burst'?['ArrowRight','ArrowRight','ArrowLeft','ArrowRight']:[];
   seekState.pendingKeys=keys.length;
   const start=performance.now();
   const frame=await new Promise((resolve,reject)=>{
     const timer=setTimeout(()=>reject(Error('seek/frame timeout '+i)),10000);
     const cb=(now,meta)=>{
       if(seekState.pendingKeys||Math.abs(meta.mediaTime-seekState.target)>2/fps) {v.requestVideoFrameCallback(cb);return}
       clearTimeout(timer);resolve({now,mediaTime:meta.mediaTime});
     };
     v.requestVideoFrameCallback(cb);
     if(keys.length)window.nvdSeekKeys(JSON.stringify(keys));else v.currentTime=seekState.target;
   });
   const latency=frame.now-start;
   ctx.drawImage(v,0,0);
   const bits=y=>{let n=0;for(let b=0;b<9;b++)n|=(ctx.getImageData(44+b*24,y,1,1).data[0]>128?1:0)<<b;return n};
   const ids=markerRows.map(bits),top=ids[0],bottom=ids.at(-1),expected=Math.round(frame.mediaTime*fps);
   if(i>=10) rows.push({i:i-10,target:seekState.target,mediaTime:frame.mediaTime,latencyMs:latency,top,bottom,ids,expected,
      correct:ids.every(id=>id===top)&&Math.abs(top-expected)<=1});
 }
 return {width:v.videoWidth,height:v.videoHeight,duration:v.duration,keyEvents,playback,quality:v.getVideoPlaybackQuality().toJSON?.()??{
   totalVideoFrames:v.getVideoPlaybackQuality().totalVideoFrames,droppedVideoFrames:v.getVideoPlaybackQuality().droppedVideoFrames},rows};
};
</script>`;
const server = http.createServer((req,res)=>{
 if(req.url !== '/video'){res.setHeader('Content-Type','text/html');res.end(html);return}
 const size=fs.statSync(videoFile).size;
 const match=/bytes=(\d+)-(\d*)/.exec(req.headers.range??'');
 const start=match?Number(match[1]):0,end=match&&match[2]?Math.min(Number(match[2]),size-1):size-1;
 res.writeHead(match?206:200,{'Content-Type':path.extname(videoFile)==='.mp4'?'video/mp4':'video/webm','Accept-Ranges':'bytes','Content-Length':end-start+1,
   ...(match?{'Content-Range':`bytes ${start}-${end}/${size}`}:{})});
 fs.createReadStream(videoFile,{start,end}).pipe(res);
});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
const port=server.address().port;
const log=fs.openSync(path.join(runDir,'chrome.log'),'w');
const env={...process.env,LIBVA_DRIVER_NAME:'nvidia',LIBVA_DRIVERS_PATH:path.resolve(driverDir),NVD_BACKEND:'direct',NVD_EXPORT_LAYOUT:layout};
for(const key of ['NVD_SINGLE_BUFFER','NVD_LOG','NVD_STATS','NVD_STATS_LOG'])delete env[key];
const diagnostic=process.env.NVD_BENCH_DIAGNOSTIC==='1';
if(diagnostic){env.NVD_STATS='final';env.NVD_STATS_LOG=path.join(runDir,'stats.log')}
const args=[`--user-data-dir=${profile}`,'--remote-debugging-port=0','--no-first-run',
 '--no-default-browser-check','--autoplay-policy=no-user-gesture-required','--disable-background-networking',
 '--enable-features=AcceleratedVideoDecodeLinuxGL,VaapiOnNvidiaGPUs','--ignore-gpu-blocklist',
 '--use-gl=angle','--use-angle=gl','--ozone-platform=wayland','about:blank'];
if(process.env.NVD_BENCH_SOFTWARE==='1')args.push('--disable-accelerated-video-decode');
const chrome=spawn(process.env.CHROME_BIN??'google-chrome-stable',args,{env,stdio:['ignore',log,log]});
chrome.on('error',e=>console.error(e));
let ws;
try {
 const active=path.join(profile,'DevToolsActivePort');
 for(let i=0;!fs.existsSync(active)&&i<150;i++)await new Promise(r=>setTimeout(r,100));
 const debugPort=fs.readFileSync(active,'utf8').split('\n')[0];
 const targets=await(await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json();
 ws=new WebSocket(targets.find(t=>t.type==='page').webSocketDebuggerUrl);
 await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject});
 let serial=0;const pending=new Map();
 ws.onmessage=e=>{
   const m=JSON.parse(e.data);
   if(m.id){const p=pending.get(m.id);if(p){pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result)}}
   else if(m.method?.startsWith('Media.'))mediaEvents.push(m);
   else if(m.method==='Runtime.bindingCalled'&&m.params.name==='nvdSeekKeys') {
    dispatchKeys(JSON.parse(m.params.payload)).catch(error=>{
      console.error(error);ws.close();
    });
   }
 };
 const send=(method,params={})=>new Promise((resolve,reject)=>{const id=++serial;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}))});
 ws.onclose=()=>{for(const p of pending.values())p.reject(Error('Chrome CDP connection closed'));pending.clear()};
 async function dispatchKeys(keys) {
  for(const [index,key] of keys.entries()) {
   const code=key==='ArrowRight'?39:37;
   await send('Input.dispatchKeyEvent',{type:'keyDown',key,code:key,windowsVirtualKeyCode:code,nativeVirtualKeyCode:code});
   await send('Input.dispatchKeyEvent',{type:'keyUp',key,code:key,windowsVirtualKeyCode:code,nativeVirtualKeyCode:code});
   if(index+1<keys.length)await new Promise(r=>setTimeout(r,10));
  }
 }
 await send('Runtime.enable');await send('Media.enable');await send('Page.enable');
 await send('Page.navigate',{url:`http://127.0.0.1:${port}/`});
 await new Promise(r=>setTimeout(r,1500));
 await send('Runtime.addBinding',{name:'nvdSeekKeys'});
 const result=await send('Runtime.evaluate',{expression:`window.runSeeks(${count})`,awaitPromise:true,returnByValue:true,timeout:180000});
 if(result.exceptionDetails)throw Error(JSON.stringify(result.exceptionDetails));
 const data=result.result.value;
 const sorted=data.rows.map(x=>x.latencyMs).sort((a,b)=>a-b);
 const percentile=p=>sorted[Math.min(sorted.length-1,Math.ceil(sorted.length*p)-1)];
 const mapped=[];
 for(const pid of fs.readdirSync('/proc').filter(x=>/^\d+$/.test(x))){
  try{if(!fs.readFileSync(`/proc/${pid}/cmdline`,'utf8').includes(profile))continue;
   const maps=fs.readFileSync(`/proc/${pid}/maps`,'utf8').split('\n').filter(x=>x.includes('nvidia_drv_video.so'));
   if(maps.length)mapped.push({pid,paths:[...new Set(maps.map(x=>x.slice(x.indexOf('/'))))]});
  }catch{}
 }
 const document={date:new Date().toISOString(),driverDir:path.resolve(driverDir),layout,mode,fps,fixture,video:path.resolve(videoFile),
   videoSha256,
   driverSha256:createHash('sha256').update(fs.readFileSync(path.join(driverDir,'nvidia_drv_video.so'))).digest('hex'),
   measurementMode:diagnostic?'diagnostic':'uninstrumented',statsLog:diagnostic?env.NVD_STATS_LOG:null,
   runDir,args,summary:{count,medianMs:percentile(.5),p95Ms:percentile(.95),p99Ms:percentile(.99),
     incorrect:data.rows.filter(x=>!x.correct).length,
     splitIds:data.rows.filter(x=>x.ids.some(id=>id!==x.top)).length},mapped,...data,mediaEvents};
 fs.writeFileSync(output,JSON.stringify(document,null,2));
 if(diagnostic) {
  // Release the decoder before terminating Chrome so final context statistics
  // are flushed even if its GPU process exits without vaTerminate.
  await send('Runtime.evaluate',{expression:"document.querySelector('#v').removeAttribute('src');document.querySelector('#v').load()"});
  await new Promise(resolve=>setTimeout(resolve,500));
 }
 console.log(JSON.stringify({output,summary:document.summary,mapped,decoder:mediaEvents.filter(x=>x.method==='Media.playerPropertiesChanged')}));
 if(process.env.NVD_BENCH_REQUIRE_CORRECT==='1') {
  const properties=mediaEvents.flatMap(x=>x.params.properties??[]);
  const hardware=properties.some(p=>p.name==='kVideoDecoderName'&&p.value==='VaapiVideoDecoder');
  const selected=path.join(path.resolve(driverDir),'nvidia_drv_video.so');
  const expectedKeys=(count+10)*(mode==='burst'?4:mode==='arrows'?1:0);
  if(document.summary.incorrect||data.rows.length!==count||data.keyEvents!==expectedKeys||
     (process.env.NVD_BENCH_SOFTWARE!=='1'&&(!hardware||!mapped.some(x=>x.paths.includes(selected)))))process.exitCode=1;
 }
} catch(error) {
 fs.writeFileSync(output,JSON.stringify({error:String(error),runDir,mediaEvents},null,2));
 console.error(error);process.exitCode=1;
} finally {
 ws?.close();chrome.kill('SIGTERM');fs.closeSync(log);server.closeAllConnections();server.close();
}
