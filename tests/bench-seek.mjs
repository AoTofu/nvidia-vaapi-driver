// Node >=22. Real headed Chrome, native CDP, private profile, no dependencies.
// Usage: node tests/bench-seek.mjs DRIVER_DIR VIDEO OUTPUT_JSON COUNT [LAYOUT]
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
const runDir = fs.mkdtempSync(path.join(os.tmpdir(), 'nvd-seek-'));
const profile = path.join(runDir, 'profile');
const mediaEvents = [];
const html = `<!doctype html><meta charset="utf-8"><title>NVIDIA seek benchmark</title>
<body style="background:#333;color:white"><h2>Dedicated seek benchmark</h2>
<video id="v" muted autoplay src="/video" style="width:960px"></video><canvas id="c" hidden></canvas>
<script>
window.runSeeks = async function(count) {
 const v = document.querySelector('#v'), c = document.querySelector('#c');
 if (v.readyState < 2) await new Promise((resolve,reject)=>{v.onloadeddata=resolve;v.onerror=()=>reject(Error('video load failed'))});
 await v.play();
 const playbackStart=performance.now(),qualityStart=v.getVideoPlaybackQuality();
 await new Promise(r=>setTimeout(r,3000));v.pause();
 const qualityEnd=v.getVideoPlaybackQuality();
 const playback={elapsedMs:performance.now()-playbackStart,
   totalFrames:qualityEnd.totalVideoFrames-qualityStart.totalVideoFrames,
   droppedFrames:qualityEnd.droppedVideoFrames-qualityStart.droppedVideoFrames};
 c.width=v.videoWidth;c.height=v.videoHeight;
 const ctx=c.getContext('2d',{willReadFrequently:true});
 const rows=[];
 for(let i=0;i<count+10;i++) {
   const target=((i*97)%330+15)/30;
   const start=performance.now();
   const frame=await new Promise((resolve,reject)=>{
     const timer=setTimeout(()=>reject(Error('seek/frame timeout '+i)),10000);
     const cb=(now,meta)=>{
       if(Math.abs(meta.mediaTime-target)>0.06) {v.requestVideoFrameCallback(cb);return}
       clearTimeout(timer);resolve({now,mediaTime:meta.mediaTime});
     };
     v.requestVideoFrameCallback(cb);v.currentTime=target;
   });
   const latency=frame.now-start;
   ctx.drawImage(v,0,0);
   const bits=y=>{let n=0;for(let b=0;b<9;b++)n|=(ctx.getImageData(44+b*24,y,1,1).data[0]>128?1:0)<<b;return n};
   const top=bits(32),bottom=bits(c.height-32),expected=Math.round(frame.mediaTime*30);
   if(i>=10) rows.push({i:i-10,target,mediaTime:frame.mediaTime,latencyMs:latency,top,bottom,expected,
      correct:top===bottom&&Math.abs(top-expected)<=1});
 }
 return {width:v.videoWidth,height:v.videoHeight,duration:v.duration,playback,quality:v.getVideoPlaybackQuality().toJSON?.()??{
   totalVideoFrames:v.getVideoPlaybackQuality().totalVideoFrames,droppedVideoFrames:v.getVideoPlaybackQuality().droppedVideoFrames},rows};
};
</script>`;
const server = http.createServer((req,res)=>{
 if(req.url !== '/video'){res.setHeader('Content-Type','text/html');res.end(html);return}
 const size=fs.statSync(videoFile).size;
 const match=/bytes=(\d+)-(\d*)/.exec(req.headers.range??'');
 const start=match?Number(match[1]):0,end=match&&match[2]?Math.min(Number(match[2]),size-1):size-1;
 res.writeHead(match?206:200,{'Content-Type':'video/webm','Accept-Ranges':'bytes','Content-Length':end-start+1,
   ...(match?{'Content-Range':`bytes ${start}-${end}/${size}`}:{})});
 fs.createReadStream(videoFile,{start,end}).pipe(res);
});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
const port=server.address().port;
const log=fs.openSync(path.join(runDir,'chrome.log'),'w');
const env={...process.env,LIBVA_DRIVER_NAME:'nvidia',LIBVA_DRIVERS_PATH:path.resolve(driverDir),NVD_BACKEND:'direct',NVD_EXPORT_LAYOUT:layout};
for(const key of ['NVD_SINGLE_BUFFER','NVD_LOG','NVD_STATS','NVD_STATS_LOG'])delete env[key];
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
 };
 const send=(method,params={})=>new Promise((resolve,reject)=>{const id=++serial;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}))});
 await send('Media.enable');await send('Page.enable');
 await send('Page.navigate',{url:`http://127.0.0.1:${port}/`});
 await new Promise(r=>setTimeout(r,1500));
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
 const document={date:new Date().toISOString(),driverDir:path.resolve(driverDir),layout,video:path.resolve(videoFile),
   videoSha256:createHash('sha256').update(fs.readFileSync(videoFile)).digest('hex'),
   driverSha256:createHash('sha256').update(fs.readFileSync(path.join(driverDir,'nvidia_drv_video.so'))).digest('hex'),
   runDir,args,summary:{count,medianMs:percentile(.5),p95Ms:percentile(.95),p99Ms:percentile(.99),
     incorrect:data.rows.filter(x=>!x.correct).length,
     splitIds:data.rows.filter(x=>x.top!==x.bottom).length},mapped,...data,mediaEvents};
 fs.writeFileSync(output,JSON.stringify(document,null,2));
 console.log(JSON.stringify({output,summary:document.summary,mapped,decoder:mediaEvents.filter(x=>x.method==='Media.playerPropertiesChanged')}));
} catch(error) {
 fs.writeFileSync(output,JSON.stringify({error:String(error),runDir,mediaEvents},null,2));
 console.error(error);process.exitCode=1;
} finally {
 ws?.close();chrome.kill('SIGTERM');fs.closeSync(log);server.closeAllConnections();server.close();
}
