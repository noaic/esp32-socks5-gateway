/* Run against the pure feedback function extracted from the production page. */
function verifyManagementFeedback(source){
 const create=new Function(source+';return createOperationFeedback;')();
 let checks=0;
 const check=(ok,label)=>{checks++;if(!ok)throw Error(label);};
 const status=(overrides={})=>Object.assign({boot_id:101,job_id:6,job_action:2,job_running:false,job_result:0,job_auto:false,pending:false},overrides);
 let f=create();
 f.begin('reboot',status(),'http://192.168.10.1/',0);
 check(f.view(0).blocked&&f.view(0).text.includes('正在提交'),'lock while submitting');
 f.accept({queued:true,restart:true,job_id:7},1);
 f.observe(status({job_id:7,job_action:3,job_running:true}),2000);
 check(f.view(2000).blocked&&f.view(2000).restarting,'same boot is not reboot completion');
 check(f.observe(status({boot_id:102}),5000)==='reboot','new boot confirms restart');
 check(!f.view(5000).blocked&&f.view(5000).text.includes('配置和口令保留'),'restart completed and unlocked');
 check(!f.view(40000).text,'completion notice expires');
 f=create();f.begin('config',status(),'http://192.168.10.1/',0);
 f.accept({reboot:true,management_url:'http://192.168.20.1/'},1);
 check(f.view(3000).otherAddress&&f.view(3000).url==='http://192.168.20.1/','new subnet link and cross-address flag');
 check(f.view(3000).text.includes('192.168.10.1'),'old rollback address retained');
 f.observe(status({boot_id:103,pending:true}),8000);
 check(f.view(8000).text.includes('待确认'),'trial requires explicit confirmation');
 f=create();f.begin('relay_reset',status(),'http://192.168.10.1/',0);
 f.accept({queued:true,job_id:7,restart:false},1);
 f.observe(status({job_id:6}),3000);
 check(f.view(3000).blocked,'stale job cannot acknowledge command');
 f.observe(status({job_id:7,job_running:true}),4000);
 check(f.view(4000).blocked,'job still running');
 f.observe(status({job_id:7}),6000);
 check(!f.view(6000).blocked&&f.view(6000).text.includes('已执行'),'exact job completion');
 f=create();f.begin('diagnose',status(),'http://192.168.10.1/',0);f.accept({queued:true,job_id:7},1);
 f.observe(status({job_id:7,job_action:4}),4000);
 check(f.view(4000).text.includes('结果见下方')&&!f.view(4000).text.includes('通过'),'finished diagnostic is not automatically a passed probe');
 f=create();f.begin('internet',status(),'http://192.168.10.1/',0);f.accept({queued:true,job_id:7},1);
 f.observe(status({job_id:7,job_result:-104}),5000);
 check(f.view(5000).text.includes('code=-104'),'task failure visible');
 f=create();f.begin('relay_reset',status(),'http://192.168.10.1/',0);f.accept({queued:true,job_id:7},1);
 f.observe(status({job_id:8,job_auto:true,job_running:true}),5000);
 check(f.view(5000).text.includes('最近事件'),'newer auto job cannot impersonate original result');
 f=create();f.begin('reboot',status(),'http://192.168.10.1/',0);
 f.reject(Error('network failed'),10);
 check(f.view(20).blocked&&f.view(20).text.includes('不会自动重复提交'),'dropped response remains uncertain and locked');
 f.observe(status({boot_id:104}),3000);
 check(!f.view(3000).blocked,'observed restart resolves dropped POST response');
 f=create();f.begin('relay_reset',status(),'http://192.168.10.1/',0);f.reject(Error('timeout'),10);
 f.observe(status({job_id:7,job_action:2,job_running:true}),2000);
 check(f.view(2000).blocked,'recover matching task after lost response');
 f.observe(status({job_id:7,job_action:2}),5000);
 check(!f.view(5000).blocked,'matching task completes after lost response');
 f=create();f.begin('reboot',status(),'http://192.168.10.1/',0);
 f.reject(Object.assign(Error('not confirmed'),{httpStatus:409}),10);
 check(!f.view(10).blocked&&f.view(10).text.includes('操作失败'),'explicit HTTP rejection unlocks');
 f=create();f.begin('reboot',status(),'http://192.168.10.1/',0);f.accept({restart:true},1);
 check(f.view(90000).blocked,'deadline before boundary');
 check(!f.view(90001).blocked&&f.view(90001).text.includes('90 秒'),'bounded waiting and no hidden retries');
 f=create();f.begin('led',status(),'http://192.168.10.1/',0);f.accept({saved:true},100);
 check(!f.view(100).blocked&&f.view(100).text.includes('完成'),'immediate settings response');
 f=create();f.begin('relay_reset',status(),'http://192.168.10.1/',0);f.accept({queued:true,job_id:7},1);
 f.observe(status({boot_id:105,job_id:7}),5000);
 check(f.view(5000).text.includes('未能确认'),'external reboot does not pretend task succeeded');
 return 'Management feedback: '+checks+' functional assertions PASS';
}

async function verifyManagementPage(script){
 let checks=0,now=1000,interval,deferred,statusReads=0,configReads=0;
 const check=(ok,label)=>{checks++;if(!ok)throw Error(label);};
 const nodes=new Map(),buttons=[];
 const node=id=>{if(!nodes.has(id))nodes.set(id,{value:'',checked:false,disabled:false,hidden:false,textContent:'',replaceChildren(){},append(){}});return nodes.get(id);};
 const fields=new Proxy({}, {get:(target,key)=>node('field-'+key)});
 node('settings').elements=fields;
 for(const id of ['refresh','confirm','defaults','saveInternet'])buttons.push(node(id));
 const actions=['reboot','reconnect','relay_reset','diagnose','internet'].map(action=>Object.assign(node('button-'+action),{dataset:{action}}));buttons.push(...actions);
 const document={hidden:false,getElementById:node,querySelectorAll:selector=>selector==='button'?buttons:actions,addEventListener(){},createElement:()=>({append(){}})};
 let status={boot_id:101,job_id:6,job_action:0,job_running:false,job_result:0,job_auto:false,control_available:true,
  clients:[],pending:false,firmware:'test-baseline',ap_ip:'192.168.10.1',internet_checked_ms:0,
  internet_probe:[],probe:[],internal_total:100000,internal_free:80000,psram_total:8000000,psram_free:7000000};
 let mode='reboot';const posts=[];
 const response=data=>({ok:true,status:200,json:async()=>data});
 const fetch=async(path,options)=>{
  if(options.method==='POST'){
   posts.push({path,payload:JSON.parse(options.body)});
   if(mode==='drop')throw Error('network failed');
   if(mode==='config')return response({reboot:true,management_url:'http://192.168.20.1/'});
   if(mode==='led')return response({saved:true});
   return response({queued:true,restart:true,job_id:7});
  }
  if(path==='/api/config'){configReads++;return response({ap_subnet:10,ap_ssid:'test-ap'});}
  if(path==='/api/events')return response({events:[]});
  if(path==='/api/status'){statusReads++;if(deferred)return new Promise(resolve=>{deferred=resolve;});return response(status);}
  throw Error('unexpected endpoint '+path);
 };
 class Controller{constructor(){this.signal={};}abort(){}}
 const api=new Function('document','window','location','fetch','AbortController','setTimeout','clearTimeout','setInterval','Date',
  script+';return {refresh,mutate,status:()=>lastStatus,feedback:()=>operationView()};')(document,{confirm:()=>true},{origin:'http://192.168.10.1'},fetch,Controller,()=>1,()=>{},callback=>{interval=callback;},{now:()=>now});
 for(let i=0;i<40;i++)await Promise.resolve();
 check(api.status().boot_id===101,'initial status loaded');
 await api.mutate('/api/action',{action:'reboot'});
 check(posts.length===1&&node('button-reboot').disabled&&!node('refresh').disabled,'accepted reboot locks mutation but allows status read');
 await api.mutate('/api/action',{action:'reboot'});
 check(posts.length===1,'duplicate reboot suppressed');
 status={...status,boot_id:102};now=5000;await api.refresh();
 check(!api.feedback().blocked&&node('operation').textContent.includes('设备已重新启动'),'actual page recovers after new boot');
 check(configReads===2,'configuration reloaded after boot');
 await api.refresh();
 check(node('operation').textContent.includes('设备已重新启动'),'normal refresh does not erase completion notice');
 mode='drop';await api.mutate('/api/action',{action:'reboot'});
 now=8000;interval();for(let i=0;i<30;i++)await Promise.resolve();
 check(posts.length===2&&api.feedback().blocked,'lost POST is not retried by polling');
 status={...status,boot_id:103};await api.refresh();
 check(!api.feedback().blocked,'new boot resolves lost POST in full page');
 mode='led';deferred=true;const stale=api.refresh();
 await Promise.resolve();await api.mutate('/api/action',{action:'led',enabled:false});
 const deliver=deferred;deferred=null;deliver(response({...status,boot_id:999,firmware:'stale'}));await stale;
 check(api.status().boot_id===103,'pre-mutation status response discarded');
 mode='config';await api.mutate('/api/config',{ap_subnet:20});
 check(node('managementLink').href==='http://192.168.20.1/'&&!node('recoveryLink').hidden,'new subnet link rendered');
 const reads=statusReads;now=12000;interval();for(let i=0;i<20;i++)await Promise.resolve();
 check(statusReads===reads,'changed subnet does not auto-query old or cross-origin endpoint');
 return 'Management full-page integration: '+checks+' functional assertions PASS';
}

if(typeof require==='function'&&typeof module!=='undefined'&&require.main===module){
 const fs=require('fs'),path=require('path');
 const script=fs.readFileSync(path.join(__dirname,'..','main','management.html'),'utf8').split('<script>')[1].split('</script>')[0];
 new Function(script);
 const source='function createOperationFeedback'+script.split('function createOperationFeedback')[1].split('\nconst operationFeedback=')[0];
 console.log(verifyManagementFeedback(source));
 verifyManagementPage(script).then(console.log).catch(error=>{console.error(error);process.exitCode=1;});
}
