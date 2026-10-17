/* Run against the pure feedback function extracted from the production page. */
function verifyManagementFeedback(source,locale){
 const expected=(english,chinese)=>locale.pageLanguage==='zh-CN'?chinese:english;
 const create=new Function('t',source+';return createOperationFeedback;')(locale.t);
 let checks=0;
 const check=(ok,label)=>{checks++;if(!ok)throw Error(label);};
 const status=(overrides={})=>Object.assign({boot_id:101,job_id:6,job_action:2,job_running:false,job_result:0,job_auto:false,pending:false},overrides);
 let f=create();
 f.begin('reboot',status(),'http://192.168.10.1/',0);
 check(f.view(0).blocked&&f.view(0).text.includes(expected('Submitting','正在提交')),'lock while submitting');
 f.accept({queued:true,restart:true,job_id:7},1);
 f.observe(status({job_id:7,job_action:3,job_running:true}),2000);
 check(f.view(2000).blocked&&f.view(2000).restarting,'same boot is not reboot completion');
 check(f.observe(status({boot_id:102}),5000)==='reboot','new boot confirms restart');
 check(!f.view(5000).blocked&&f.view(5000).text.includes(expected('Configuration and password are retained','配置和口令保留')),'restart completed and unlocked');
 check(!f.view(40000).text,'completion notice expires');
 f=create();f.begin('config',status(),'http://192.168.10.1/',0);
 f.accept({reboot:true,management_url:'http://192.168.20.1/'},1);
 check(f.view(3000).otherAddress&&f.view(3000).url==='http://192.168.20.1/','new subnet link and cross-address flag');
 check(f.view(3000).text.includes('192.168.10.1'),'old rollback address retained');
 f.observe(status({boot_id:103,pending:true}),8000);
 check(f.view(8000).text.includes(expected('awaits confirmation','待确认')),'trial requires explicit confirmation');
 f=create();f.begin('relay_reset',status(),'http://192.168.10.1/',0);
 f.accept({queued:true,job_id:7,restart:false},1);
 f.observe(status({job_id:6}),3000);
 check(f.view(3000).blocked,'stale job cannot acknowledge command');
 f.observe(status({job_id:7,job_running:true}),4000);
 check(f.view(4000).blocked,'job still running');
 f.observe(status({job_id:7}),6000);
 check(!f.view(6000).blocked&&f.view(6000).text.includes(expected('was executed','已执行')),'exact job completion');
 f=create();f.begin('diagnose',status(),'http://192.168.10.1/',0);f.accept({queued:true,job_id:7},1);
 f.observe(status({job_id:7,job_action:4}),4000);
 check(f.view(4000).text.includes(expected('see the results below','结果见下方'))&&!f.view(4000).text.includes(expected('passed','通过')),'finished diagnostic is not automatically a passed probe');
 f=create();f.begin('internet',status(),'http://192.168.10.1/',0);f.accept({queued:true,job_id:7},1);
 f.observe(status({job_id:7,job_result:-104}),5000);
 check(f.view(5000).text.includes('code=-104'),'task failure visible');
 f=create();f.begin('relay_reset',status(),'http://192.168.10.1/',0);f.accept({queued:true,job_id:7},1);
 f.observe(status({job_id:8,job_auto:true,job_running:true}),5000);
 check(f.view(5000).text.includes(expected('recent events','最近事件')),'newer auto job cannot impersonate original result');
 f=create();f.begin('reboot',status(),'http://192.168.10.1/',0);
 f.reject(Error('network failed'),10);
 check(f.view(20).blocked&&f.view(20).text.includes(expected('not be automatically resubmitted','不会自动重复提交')),'dropped response remains uncertain and locked');
 f.observe(status({boot_id:104}),3000);
 check(!f.view(3000).blocked,'observed restart resolves dropped POST response');
 f=create();f.begin('relay_reset',status(),'http://192.168.10.1/',0);f.reject(Error('timeout'),10);
 f.observe(status({job_id:7,job_action:2,job_running:true}),2000);
 check(f.view(2000).blocked,'recover matching task after lost response');
 f.observe(status({job_id:7,job_action:2}),5000);
 check(!f.view(5000).blocked,'matching task completes after lost response');
 f=create();f.begin('reboot',status(),'http://192.168.10.1/',0);
 f.reject(Object.assign(Error('not confirmed'),{httpStatus:409}),10);
 check(!f.view(10).blocked&&f.view(10).text.includes(expected('Operation failed','操作失败')),'explicit HTTP rejection unlocks');
 f=create();f.begin('reboot',status(),'http://192.168.10.1/',0);f.accept({restart:true},1);
 check(f.view(90000).blocked,'deadline before boundary');
 check(!f.view(90001).blocked&&f.view(90001).text.includes(expected('90 seconds','90 秒')),'bounded waiting and no hidden retries');
 f=create();f.begin('led',status(),'http://192.168.10.1/',0);f.accept({saved:true},100);
 check(!f.view(100).blocked&&f.view(100).text.includes(expected('completed','完成')),'immediate settings response');
 f=create();f.begin('relay_reset',status(),'http://192.168.10.1/',0);f.accept({queued:true,job_id:7},1);
 f.observe(status({boot_id:105,job_id:7}),5000);
 check(f.view(5000).text.includes(expected('could not be confirmed','未能确认')),'external reboot does not pretend task succeeded');
 for(const [kind,payload,before,after]of [
  ['led',{enabled:false},{led_enabled:true},{led_enabled:false}],
  ['internet_settings',{enabled:false,target:1},{internet_auto:true,internet_target:0},{internet_auto:false,internet_target:1}],
  ['confirm',{}, {pending:true},{pending:false}],
  ['kick',{token:77},{clients:[{token:77}]},{clients:[]}]
 ]){
  f=create();f.begin(kind,status(before),'http://192.168.10.1/',0,payload);
  check(f.view(0).blocked,'instant request is protected while sending: '+kind);
  f.reject(Error('network failed'),1);f.observe(status(before),10);
  check(!f.view(10).blocked&&f.view(10).text.includes(expected('other operations remain available','其他操作仍可使用')),'uncertain instant request leaves confirmation available: '+kind);
  f.observe(status(after),20);
  check(!f.view(20).blocked&&f.view(20).text.includes(expected('now visible','已达到')),'actual desired state resolves lost response: '+kind);
 }
 return 'Management feedback ('+locale.pageLanguage+'): '+checks+' functional assertions PASS';
}

async function verifyManagementPage(script,browser){
 const expected=(english,chinese)=>browser.languages[0].startsWith('zh')?chinese:english;
 let checks=0,now=1000,interval,deferred,statusReads=0,configReads=0;
 const check=(ok,label)=>{checks++;if(!ok)throw Error(label);};
 const nodes=new Map(),buttons=[];
 const node=id=>{if(!nodes.has(id))nodes.set(id,{value:'',checked:false,disabled:false,hidden:false,open:false,textContent:'',replaceChildren(){},append(){},addEventListener(event,listener){this[event]=listener;},contains(child){return child.advanced===true;}});return nodes.get(id);};
 const fields=new Proxy({}, {get:(target,key)=>node('field-'+key)});
 node('settings').elements=fields;
 for(const id of ['refresh','confirm','defaults','saveInternet','guideAction'])buttons.push(node(id));
 const actions=['reboot','reconnect','relay_reset','diagnose','internet'].map(action=>Object.assign(node('button-'+action),{dataset:{action}}));buttons.push(...actions);
 const document={documentElement:{lang:'en'},hidden:false,getElementById:node,querySelectorAll:selector=>selector==='button'?buttons:selector==='[data-action]'?actions:[],addEventListener(){},createElement:()=>({append(){}})};
 let status={boot_id:101,job_id:6,job_action:0,job_running:false,job_result:0,job_auto:false,control_available:true,
  clients:[],pending:false,firmware:'test-baseline',ap_ip:'192.168.10.1',ap_client_limit:2,sta_connected:true,forwarding_ready:true,sta_ip:'192.168.1.10',internet_checked_ms:0,internet_state:0,tcp_flow_limit:64,udp_flow_limit:32,
  internet_auto:true,internet_target:0,internet_probe:[],probe:[],internal_total:100000,internal_free:80000,psram_total:8000000,psram_free:7000000};
 let mode='reboot';const posts=[];
 const response=data=>({ok:true,status:200,json:async()=>data});
 const fetch=async(path,options)=>{
  if(options.method==='POST'){
   posts.push({path,payload:JSON.parse(options.body)});
   if(mode==='drop')throw Error('network failed');
   if(mode==='config')return response({reboot:true,management_url:'http://192.168.20.1/'});
   if(mode==='led')return response({saved:true});
   if(mode==='internet'){const payload=JSON.parse(options.body);status={...status,internet_auto:payload.enabled,internet_target:payload.target};return response({saved:true});}
   if(mode==='instant-not-applied')throw Error('network failed');
   if(mode==='instant-drop'){status={...status,led_enabled:JSON.parse(options.body).enabled};throw Error('network failed');}
   if(mode==='confirm'){status={...status,pending:false};return response({confirmed:true});}
   return response({queued:true,restart:true,job_id:7});
  }
  if(path==='/api/config'){configReads++;return response({ap_subnet:10,ap_ssid:'test-ap',sta_ssid:'upstream'});}
  if(path==='/api/events')return response({events:[{event_code:'boot',event:'设备启动',last_ms:1000,detail:0,count:1},{event:'STA 已关联',last_ms:2000,detail:0,count:1}]});
  if(path==='/api/status'){statusReads++;if(deferred)return new Promise(resolve=>{deferred=resolve;});return response(status);}
  throw Error('unexpected endpoint '+path);
 };
 class Controller{constructor(){this.signal={};}abort(){}}
 const api=new Function('navigator','document','window','location','fetch','AbortController','setTimeout','clearTimeout','setInterval','Date',
  script+';return {refresh,mutate,status:()=>lastStatus,feedback:()=>operationView()};')(browser,document,{confirm:()=>true},{origin:'http://192.168.10.1'},fetch,Controller,()=>1,()=>{},callback=>{interval=callback;},{now:()=>now});
 for(let i=0;i<40;i++)await Promise.resolve();
 check(api.status().boot_id===101,'initial status loaded');
 check(!node('configDetails').open&&!node('advancedSettings').open&&!node('diagnosticsDetails').open&&!node('statusDetails').open&&!node('maintenanceDetails').open,'configured healthy page keeps advanced sections folded');
 check(node('nextTitle').textContent===expected('Wi-Fi is ready; verify the proxy path','Wi-Fi 已就绪，请验证代理链路'),'initial primary action explains next step');
 status={...status,subnet_overlap:true};await api.refresh();
 check(node('configDetails').open&&node('advancedSettings').open,'subnet conflict opens the settings required to fix it');
 node('configDetails').open=false;node('advancedSettings').open=false;await api.refresh();
 check(!node('configDetails').open&&!node('advancedSettings').open,'polling respects the user collapsing an already-reported issue');
 await node('guideAction').onclick();
 check(node('configDetails').open&&node('advancedSettings').open&&posts.length===0,'guided settings action navigates without a mutation');
 node('configDetails').open=false;node('advancedSettings').open=false;
 node('settings').invalid({target:{advanced:true}});
 check(node('configDetails').open&&node('advancedSettings').open,'invalid required fields become focusable inside folded settings');
 status={...status,subnet_overlap:false,internet_checked_ms:100,internet_state:3,internet_probe:[{code:0,ms:1},{code:0,ms:1},{code:0,ms:1},{code:6302,ms:1}]};await api.refresh();
 check(node('diagnosticsDetails').open&&node('nextHelp').textContent.includes(expected('does not establish that all Internet access is down','不表示整个互联网都不可用')),'target failure opens diagnostics with limited and actionable interpretation');
 node('diagnosticsDetails').open=false;await api.refresh();
 check(!node('diagnosticsDetails').open,'repeated polling does not reopen the same diagnostic result');
 status={...status,internet_checked_ms:0,internet_state:0,internet_probe:[]};await api.refresh();
 check(node('events').textContent.includes(expected('Device started','设备启动'))&&node('events').textContent.includes(expected('STA associated','STA 已关联')),'coded and legacy events follow page language');
 node('internetAuto').checked=false;node('internetAuto').onchange();node('internetTarget').value='1';node('internetTarget').onchange();
 await api.refresh();
 check(node('internetAuto').checked===false&&node('internetTarget').value==='1','status refresh preserves unsaved check settings');
 mode='internet';await api.mutate('/api/action',{action:'internet_settings',enabled:false,target:1});
 check(!node('internetAuto').checked&&node('internetTarget').value==='1','successful save updates the server and clears the draft');
 status={...status,internet_auto:true,internet_target:0};await api.refresh();
 check(node('internetAuto').checked&&node('internetTarget').value==='0','saved controls follow later server updates');mode='reboot';
 await api.mutate('/api/action',{action:'reboot'});
 check(posts.length===2&&node('button-reboot').disabled&&!node('refresh').disabled,'accepted reboot locks mutation but allows status read');
 await api.mutate('/api/action',{action:'reboot'});
 check(posts.length===2,'duplicate reboot suppressed');
 status={...status,boot_id:102};now=5000;await api.refresh();
 check(!api.feedback().blocked&&node('operation').textContent.includes(expected('The device restarted','设备已重新启动')),'actual page recovers after new boot');
 check(configReads===2,'configuration reloaded after boot');
 await api.refresh();
 check(node('operation').textContent.includes(expected('The device restarted','设备已重新启动')),'normal refresh does not erase completion notice');
 mode='drop';await api.mutate('/api/action',{action:'reboot'});
 now=8000;interval();for(let i=0;i<30;i++)await Promise.resolve();
 check(posts.length===3&&api.feedback().blocked,'lost POST is not retried by polling');
 status={...status,boot_id:103};await api.refresh();
 check(!api.feedback().blocked,'new boot resolves lost POST in full page');
 mode='led';deferred=true;const stale=api.refresh();
 await Promise.resolve();await api.mutate('/api/action',{action:'led',enabled:false});
 const deliver=deferred;deferred=null;deliver(response({...status,boot_id:999,firmware:'stale'}));await stale;
 check(api.status().boot_id===103,'pre-mutation status response discarded');
 status={...status,pending:true,led_enabled:true};await api.refresh();
 const beforeInstant=posts.length;mode='instant-not-applied';await api.mutate('/api/action',{action:'led',enabled:false});
 check(!api.feedback().blocked&&!node('confirm').disabled,'lost LED response cannot block the configuration deadline');
 check(posts.length===beforeInstant+1,'uncertain instant operation is never automatically replayed');
 mode='confirm';await api.mutate('/api/confirm',{});
 check(!api.status().pending&&posts.length===beforeInstant+2,'configuration can be confirmed while an instant result is uncertain');
 mode='instant-drop';await api.mutate('/api/action',{action:'led',enabled:false});
 check(!api.feedback().blocked&&node('operation').textContent.includes(expected('now visible','已达到')),'full page observes saved LED state after lost response');
 mode='config';await api.mutate('/api/config',{ap_subnet:20});
 check(node('managementLink').href==='http://192.168.20.1/'&&!node('recoveryLink').hidden,'new subnet link rendered');
 const reads=statusReads;now=12000;interval();for(let i=0;i<20;i++)await Promise.resolve();
 check(statusReads===reads,'changed subnet does not auto-query old or cross-origin endpoint');
 return 'Management full-page integration ('+document.documentElement.lang+'): '+checks+' functional assertions PASS';
}

function verifyConnectionGuidance(locale){
 let checks=0;
 const check=(ok,label)=>{checks++;if(!ok)throw Error(label);};
 const base={sta_connected:true,forwarding_ready:true,subnet_overlap:false,job_id:0,job_running:false,job_action:0,job_result:0,
  internet_checked_ms:0,internet_state:0,clients:[]};
 const config={sta_ssid:'upstream'};
 const steps=(stage,code,count=3)=>Array.from({length:count},(_,i)=>({code:i<stage?0:i===stage?code:5000,ms:1}));
 const expect=(overrides,kind,action,settings=config)=>{
  const result=locale.connectionGuidance({...base,...overrides},settings);
  check(result.kind===kind&&result.action===action,'guidance '+kind+' / '+action);
  check(locale.t('guide_'+kind+'_title').length>0&&locale.t('guide_'+kind+'_help').length>0&&locale.t('guide_'+action).length>0,'localized guidance '+kind);
  check(!['reboot','relay_reset','reconnect','confirm'].includes(result.action),'guidance never proposes disruptive or premature confirmation');
 };
 expect({},'unread','read_config',null);
 expect({},'unread','read_config',{});
 expect({},'setup','settings',{sta_ssid:''});
 expect({sta_connected:false},'wifi','settings');
 expect({subnet_overlap:true,sta_connected:false},'subnet','advanced');
 expect({forwarding_ready:false},'wait','details');
 expect({job_running:true},'busy','diagnostics');
 expect({pending:true},'trial','internet');
 expect({clients:[{ip:'0.0.0.0'}]},'dhcp','details');
 expect({internet_state:2,internet_checked_ms:100},'pass','diagnostics');
 expect({tcp_errors:999,tcp_error:-104,udp_errors:999,udp_error:-104},'check','internet');
 for(const [stage,code,kind] of [[0,-111,'port'],[1,2255,'auth'],[1,3000,'auth'],[2,-110,'udp'],[2,1007,'udp'],[2,4003,'dns']]){
  expect({job_action:4,job_id:3,probe:steps(stage,code)},kind,'diagnose');
  expect({internet_checked_ms:100,internet_state:0,internet_probe:steps(stage,code,4)},kind,'internet');
 }
 for(const code of [-110,1002,6302,7000,3000])expect({internet_checked_ms:100,internet_state:3,internet_probe:steps(3,code,4)},'target','internet');
 expect({internet_checked_ms:100,internet_state:3,internet_probe:steps(3,2255,4)},'auth','internet');
 expect({job_action:4,job_id:3,probe:steps(3,0)},'probe_pass','internet');
 expect({job_action:4,job_id:3,probe:steps(0,5000)},'check','internet');
 expect({job_action:4,job_id:3,job_result:259,probe:steps(0,5000)},'check','internet');
 expect({internet_checked_ms:0,internet_state:0,internet_probe:steps(0,-111,4)},'check','internet');
 expect({internet_checked_ms:100,internet_state:4,internet_probe:steps(0,-111,4)},'check','internet');
 expect({internet_checked_ms:100,internet_state:0,internet_probe:steps(0,5000,4)},'check','internet');
 check(!locale.t('guide_target_help').includes('reboot'),'target failure does not suggest rebooting');
 return 'Management actionable guidance ('+locale.pageLanguage+'): '+checks+' functional assertions PASS';
}

function pageI18n(script,browser){
 const document={documentElement:{lang:'en'},querySelectorAll:()=>[]};
 return new Function('navigator','document',script.slice(0,script.indexOf('const el=id=>'))+
  ';return {t,pageLanguage,responseError,connectionGuidance:typeof connectionGuidance==="function"?connectionGuidance:null,settingsValidation:typeof settingsValidation==="function"?settingsValidation:null};')(browser,document);
}
function verifyUTF8Settings(locale){
 let checks=0;const check=(ok,label)=>{checks++;if(!ok)throw Error(label);};
 const base={sta_ssid:'router',ap_ssid:'gateway',sta_password:'',ap_password:'',admin_password:''};
 const valid=values=>locale.settingsValidation({...base,...values})===null;
 check(valid({admin_password:'管理测试'}),'four Chinese characters meet the 12-byte password minimum');
 check(valid({admin_password:'😀'.repeat(16)}),'64-byte management password is accepted');
 check(!valid({admin_password:'😀'.repeat(17)}),'68-byte management password is rejected');
 check(!valid({admin_password:'管理测'}),'9-byte management password is rejected');
 check(valid({sta_ssid:'中'.repeat(10)+'ab',ap_ssid:'中'.repeat(10)+'ab'}),'SSID 32-byte boundary is accepted');
 check(!valid({sta_ssid:'中'.repeat(11)}),'33-byte upstream SSID is rejected');
 check(!valid({ap_ssid:'中'.repeat(11)}),'33-byte AP SSID is rejected');
 check(valid({ap_password:'中文ab'}),'8-byte AP password is accepted');
 check(!valid({ap_password:'中文a'}),'7-byte AP password is rejected');
 check(valid({ap_password:'中'.repeat(21)}),'63-byte AP password is accepted');
 check(!valid({ap_password:'中'.repeat(21)+'a'}),'64-byte AP password is rejected');
 check(valid({sta_password:'a'.repeat(64)}),'64 hexadecimal PSK bytes are accepted');
 check(!valid({sta_password:'g'.repeat(64)}),'64 nonhexadecimal PSK bytes are rejected');
 check(valid({sta_open:true,sta_password:'ignored'}),'explicit open Wi-Fi ignores entered password');
 check(valid({sta_ssid:'',sta_password:'',ap_password:'',admin_password:''}),'AP-only mode and keep-password blanks remain valid');
 check(!valid({ap_ssid:''}),'AP SSID cannot be empty');
 check(!valid({admin_password:'password\u0001here'}),'control characters are rejected');
 return 'Management UTF-8 settings ('+locale.pageLanguage+'): '+checks+' boundary assertions PASS';
}
function verifyLanguageSelection(pages){
 let checks=0;
 const check=(ok,label)=>{checks++;if(!ok)throw Error(label);};
 const cases=[
  [{languages:['en-US','zh-CN'],language:'zh-CN'},'en'],
  [{languages:['zh-CN','en-US'],language:'en-US'},'zh-CN'],
  [{languages:['zh-TW']},'zh-CN'],
  [{languages:['ZH-Hans-CN']},'zh-CN'],
  [{languages:['zh']},'zh-CN'],
  [{languages:['ja-JP','zh-CN']},'en'],
  [{languages:[],language:'zh-CN'},'zh-CN'],
  [{language:'en-GB'},'en'],
  [{language:'zh_CN'},'en'],
  [{language:null},'en'],
  [{get languages(){throw Error('unavailable');},language:'zh-CN'},'zh-CN'],
  [{get languages(){throw Error('unavailable');},get language(){throw Error('unavailable');}},'en'],
  [undefined,'en']
 ];
 for(const page of pages)for(const [browser,language]of cases){
  const locale=pageI18n(page,browser);
  check(locale.pageLanguage===language,'first preference and English fallback');
  const auth=locale.responseError({error_code:'AUTH_REQUIRED',error:'需要管理密码'},401);
  check(auth.message===(language==='en'?'Management password required':'需要管理密码')&&auth.httpStatus===401,'localized code with original HTTP status');
  const legacy=locale.responseError({error:'需要管理密码'},401);
  check(legacy.message===auth.message,'older firmware errors are translated');
  const unknown=locale.responseError({error_code:'toString',error:'未识别的错误'},409);
  check(unknown.message===(language==='en'?'Request failed (HTTP 409)':'请求失败（HTTP 409）'),'unknown error uses selected language');
  const malformed=locale.responseError(null,503);
  check(malformed.httpStatus===503&&malformed.message===(language==='en'?'Request failed (HTTP 503)':'请求失败（HTTP 503）'),'malformed HTTP error retains status');
 }
 return 'Management language selection: '+checks+' functional assertions PASS';
}
if(typeof module!=='undefined')module.exports={pageI18n};
if(typeof require==='function'&&typeof module!=='undefined'&&require.main===module){
 const fs=require('fs'),path=require('path');
 const pages=['management.html','management_setup.html'].map(file=>fs.readFileSync(path.join(__dirname,'..','main',file),'utf8').split('<script>')[1].split('</script>')[0]);
 for(const page of pages)new Function(page);
 console.log(verifyLanguageSelection(pages));
 const script=pages[0];
 const source='function createOperationFeedback'+script.split('function createOperationFeedback')[1].split('\nconst operationFeedback=')[0];
 (async()=>{
  for(const language of ['en-US','zh-CN']){
   const browser={languages:[language],language};
   console.log(verifyManagementFeedback(source,pageI18n(script,browser)));
   console.log(verifyUTF8Settings(pageI18n(script,browser)));
   console.log(verifyConnectionGuidance(pageI18n(script,browser)));
   console.log(await verifyManagementPage(script,browser));
  }
 })().catch(error=>{console.error(error);process.exitCode=1;});
}
