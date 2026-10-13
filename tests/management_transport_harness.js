/* Executes the actual page transport with missing/new browser APIs. */
async function verifyManagementTransport(helper,locale) {
 const check=(value,message)=>{if(!value)throw Error(message);};
 const make=new Function('t','responseError','AbortController','AbortSignal','XMLHttpRequest','fetch','setTimeout','clearTimeout',helper+';return requestJSON;');
 const factory=(...args)=>make(locale.t,locale.responseError,...args);
 let cleared=0,scheduled,seen;
 class Controller {constructor(){this.signal={};}abort(){if(this.signal.cancel)this.signal.cancel();}}
 const schedule=callback=>{scheduled=callback;return 1;};const clear=()=>{cleared++;};
 let transport=factory(Controller,undefined,undefined,async(path,options)=>{seen={path,options};return {ok:true,json:async()=>({ok:1})};},schedule,clear);
 check((await transport('/api/status')).ok===1,'fetch GET without AbortSignal.timeout');
 check(seen.options.credentials==='same-origin' && seen.options.method==='GET','credential forwarding');
 check(cleared===1,'clear GET deadline');
 await transport('/api/config',{max_clients:2});
 check(seen.options.headers['X-Proxy-Action']==='manage' && seen.options.headers['Content-Type']==='application/json','mutation headers');
 check(seen.options.body==='{"max_clients":2}','mutation JSON');
 transport=factory(Controller,undefined,undefined,async()=>({ok:false,status:401,json:async()=>({error:'login required'})}),schedule,clear);
 try{await transport('/api/status');throw Error('expected auth failure');}catch(error){check(error.message==='login required'&&error.httpStatus===401,'auth error and HTTP status propagation');}
 transport=factory(Controller,undefined,undefined,async(path,options)=>new Promise((resolve,reject)=>{options.signal.cancel=()=>reject(Error('aborted'));}),schedule,clear);
 const timed=transport('/api/status');scheduled();
 try{await timed;throw Error('expected deadline');}catch(error){check(error.message===locale.t('timeout'),'fetch deadline');}
 transport=factory(Controller,undefined,undefined,async()=>{throw new TypeError('native network error');},schedule,clear);
 try{await transport('/api/status');throw Error('expected network error');}catch(error){check(error.message===locale.t('network_failed')&&!error.httpStatus,'native fetch errors are localized and remain uncertain');}
 transport=factory(Controller,undefined,undefined,async()=>({ok:true,json:async()=>{throw new SyntaxError('native JSON error');}}),schedule,clear);
 try{await transport('/api/status');throw Error('expected JSON error');}catch(error){check(error.message===locale.t('invalid_response')&&!error.httpStatus,'fetch malformed responses are localized and remain uncertain');}
 class XHR {
  constructor(){this.headers={};seen=this;}
  open(method,path){this.method=method;this.path=path;}
  setRequestHeader(key,value){this.headers[key]=value;}
  send(body){this.body=body;this.status=200;this.responseText='{"ok":2}';this.onload();}
 }
 transport=factory(undefined,undefined,XHR,()=>{throw Error('fetch must not be used');},schedule,clear);
 check((await transport('/api/status')).ok===2,'XHR fallback GET');
 check(seen.body===null && seen.timeout===8000 && seen.withCredentials===true,'XHR timeout and credentials');
 await transport('/api/setup',{password:'example-password',confirm:'example-password'});
 check(seen.method==='POST' && seen.headers['X-Proxy-Action']==='manage' && JSON.parse(seen.body).password==='example-password','XHR setup payload');
 class TimeoutXHR extends XHR {send(){this.ontimeout();}}
 transport=factory(undefined,undefined,TimeoutXHR,undefined,schedule,clear);
 try{await transport('/api/status');throw Error('expected XHR timeout');}catch(error){check(error.message===locale.t('timeout'),'XHR deadline');}
 class RejectedXHR extends XHR {send(){this.status=409;this.responseText='{"error":"busy"}';this.onload();}}
 transport=factory(undefined,undefined,RejectedXHR,undefined,schedule,clear);
 try{await transport('/api/config',{});throw Error('expected XHR rejection');}catch(error){check(error.message==='busy'&&error.httpStatus===409,'XHR HTTP rejection distinguished from transport failure');}
 class MalformedXHR extends XHR {send(){this.status=200;this.responseText='not JSON';this.onload();}}
 transport=factory(undefined,undefined,MalformedXHR,undefined,schedule,clear);
 try{await transport('/api/config',{});throw Error('expected XHR JSON error');}catch(error){check(error.message===locale.t('invalid_response')&&!error.httpStatus,'XHR malformed responses are localized and remain uncertain');}
 return 'Management transport ('+locale.pageLanguage+'): missing AbortSignal.timeout, fetch/XHR, credentials, mutation headers and deadlines PASS';
}
if(typeof require==='function' && typeof module!=='undefined' && require.main===module){
 const fs=require('fs'),path=require('path');
 const pages=['management.html','management_setup.html'].map(file=>fs.readFileSync(path.join(__dirname,'..','main',file),'utf8'));
 const scripts=pages.map(page=>page.split('<script>')[1].split('</script>')[0]);
 const {pageI18n}=require('./management_feedback_harness');
 const helpers=pages.map(page=>page.split('async function requestJSON')[1].split('\n}\n')[0]);
 if(helpers[0]!==helpers[1])throw Error('Page transport helpers differ');
 (async()=>{for(const language of ['en-US','zh-CN'])for(const script of scripts){
  const locale=pageI18n(script,{languages:[language],language});
  console.log(await verifyManagementTransport('async function requestJSON'+helpers[0]+'\n}\n',locale));
 }})().catch(error=>{console.error(error);process.exitCode=1;});
}
