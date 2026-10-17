/* Optional real-browser regression: requires the host-only Playwright package
 * and its Chromium browser. No ESP32 or real network endpoints are used. */
'use strict';
const {chromium}=require('playwright');
const fs=require('fs');
const path=require('path');
const assert=require('assert/strict');
const pages=path.join(__dirname,'..','main');
const management=fs.readFileSync(path.join(pages,'management.html'),'utf8');
const setup=fs.readFileSync(path.join(pages,'management_setup.html'),'utf8');
const baseConfig={sta_ssid:'test-wifi',sta_password_set:true,ap_ssid:'test-ap',
 socks_ipv4:'192.168.1.2',socks_port:7897,max_clients:2,dns_ipv4:'1.1.1.1',ap_subnet:4};
const baseStatus={boot_id:101,job_id:0,job_action:0,job_running:false,job_result:0,job_auto:false,control_available:true,
 clients:[],pending:false,firmware:'test-build',ap_ip:'192.168.4.1',ap_netmask:'255.255.255.0',ap_client_limit:2,
 sta_ip:'192.168.1.100',sta_netmask:'255.255.255.0',sta_connected:true,sta_rssi:-40,channel:6,uptime_seconds:100,
 forwarding_ready:true,sta_disconnect_reason:0,subnet_overlap:false,internet_auto:true,internet_target:0,
 internet_checked_ms:95000,internet_success_ms:95000,internet_failures:0,internet_state:2,
 internet_url:'http://www.msftconnecttest.com/connecttest.txt',internet_valid_seconds:600,internet_next_ms:0,
 internet_probe:[0,0,0,0].map(code=>({code,ms:1})),probe:[],internal_total:100000,internal_free:80000,internal_min:70000,
 internal_largest:60000,psram_total:8000000,psram_free:7000000,http_stack_total:6144,http_stack_min:2000,
 control_stack_min:1000,control_stack_total:6144,tcp_flow_limit:64,udp_flow_limit:32,tcp_pcb_limit:256,
 tcp_pcb_live:4,tcp_pcb_peak:5,tcp_pcb_failures:0,ap_tx_errors:0,sta_tx_errors:0,clone_failures:0,
 client_rto:0,upstream_rto:0,client_fast:0,upstream_fast:0,reset_reason:1,led_enabled:true,led_ready:true,
 led_error:0,led_color:3,verbose_diagnostics:false,automatic_link_probe:false};

(async()=>{
 const browser=await chromium.launch({headless:true,args:['--no-sandbox']});
 try{
  for(const locale of ['en-US','zh-CN'])for(const width of [1100,390]){
   const context=await browser.newContext({locale,viewport:{width,height:850}});
   const page=await context.newPage(),errors=[],consoleErrors=[];
   page.on('pageerror',error=>errors.push(error.message));
   page.on('console',message=>{if(message.type()==='error')consoleErrors.push(message.text());});
   page.on('dialog',dialog=>dialog.accept());
   let config={...baseConfig},status={...baseStatus},reject=false,dropImmediate='',posts=[],checks=0;
   const check=(condition,label)=>{checks++;assert(condition,locale+'/'+width+': '+label);};
   await page.route('**/*',async route=>{
    const request=route.request(),url=new URL(request.url());
    check(url.origin==='http://192.168.4.1','requests remain on the management origin');
    if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:management});
    if(url.pathname==='/favicon.ico')return route.fulfill({status:204});
    let data;
    if(request.method()==='POST'){
     const payload=request.postDataJSON();posts.push({path:url.pathname,payload});
     check(request.headers()['x-proxy-action']==='manage','POST preserves management marker');
     if(reject)return route.fulfill({status:409,contentType:'application/json',body:'{"code":"CONTROL_UNAVAILABLE","error":"busy"}'});
     if(payload.action==='internet_settings'){
      status={...status,internet_auto:payload.enabled,internet_target:payload.target};data={saved:true};
      if(dropImmediate)return route.abort('failed');
     }else if(payload.action==='led'){
      if(dropImmediate!=='unapplied')status={...status,led_enabled:payload.enabled};
      if(dropImmediate)return route.abort('failed');
      data={saved:true};
     }else if(url.pathname==='/api/confirm'){
      status={...status,pending:false};data={confirmed:true};
     }else if(url.pathname==='/api/config'){
      data={reboot:true,confirm_seconds:180,management_url:'http://192.168.4.1/'};
     }else if(payload.action==='diagnose'||payload.action==='internet'){
      status={...status,job_id:status.job_id+1,job_action:payload.action==='diagnose'?4:5,
       job_running:false,job_result:0,probe:[0,0,0].map(code=>({code,ms:1}))};
      data={queued:true,job_id:status.job_id,restart:false};
     }else throw Error('Unexpected mutation '+url.pathname);
    }else if(url.pathname==='/api/config')data=config;
    else if(url.pathname==='/api/status')data=status;
    else if(url.pathname==='/api/events')data={events:[
     {event_code:'boot',event:'设备启动',last_ms:1000,detail:0,count:1},
     {event:'STA 已关联',last_ms:2000,detail:0,count:1},
     {event:'<img src=x onerror="window.injected=true">',last_ms:3000,detail:0,count:1}]};
    else throw Error('Unexpected request '+url.pathname);
    return route.fulfill({contentType:'application/json',body:JSON.stringify(data)});
   });
   const idle=()=>page.waitForFunction(()=>lastStatus&&!busy&&!polling);
   const apply=async(patch={},settings={})=>{
    status={...baseStatus,...patch};config={...baseConfig,...settings};
    await idle();await page.evaluate(async()=>{await loadConfig();await refresh();});await idle();
   };
   const kind=()=>page.evaluate(()=>connectionGuidance(lastStatus,loadedConfig).kind);
   await page.goto('http://192.168.4.1/');await idle();
   check(await page.locator('html').getAttribute('lang')===(locale==='zh-CN'?'zh-CN':'en'),'page language');
   check(await kind()==='pass','successful recent check is shown');
   for(const id of ['configDetails','diagnosticsDetails','statusDetails','maintenanceDetails'])
    check(!await page.locator('#'+id).evaluate(node=>node.open),'ready page initially folds '+id);
   check(await page.locator('#overview').isVisible()&&await page.locator('#nextStep').isVisible(),'overview and next step stay visible');
   check(await page.locator('#message').evaluate(node=>getComputedStyle(node).position)==='static','routine status stays in document flow');
   check(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'overview fits viewport');
   check(!/[\u3400-\u9fff]/.test(await page.locator('body').innerText())||locale==='zh-CN','English visible copy has no Chinese fallback');
   if(process.env.GATEWAY_BROWSER_SCREENSHOTS){
    fs.mkdirSync(process.env.GATEWAY_BROWSER_SCREENSHOTS,{recursive:true});
    await page.screenshot({path:path.join(process.env.GATEWAY_BROWSER_SCREENSHOTS,locale+'-'+width+'.png'),fullPage:true});
   }
   const countBefore=posts.length;await page.locator('#guideAction').click();
   check(await page.locator('#diagnosticsDetails').evaluate(node=>node.open),'primary result action opens diagnostics');
   check(posts.length===countBefore,'opening results does not mutate state');
   const events=await page.locator('#events').textContent();
   check(events.includes(locale==='zh-CN'?'设备启动':'Device started')&&events.includes(locale==='zh-CN'?'STA 已关联':'STA associated'),'coded and legacy events translate');
   check(await page.locator('#events img').count()===0&&await page.evaluate(()=>window.injected)===undefined,'events render as safe text');
   await page.locator('#internetAuto').uncheck();await page.locator('#internetTarget').selectOption('1');
   await page.evaluate(()=>refresh());await idle();
   check(!await page.locator('#internetAuto').isChecked()&&await page.locator('#internetTarget').inputValue()==='1','polling preserves edited preferences');
   await page.locator('#saveInternet').click();await idle();
   check(!await page.evaluate(()=>internetDraft)&&!status.internet_auto&&status.internet_target===1,'save commits and clears draft');
   status={...status,internet_auto:true,internet_target:0};await page.evaluate(()=>refresh());await idle();
   check(await page.locator('#internetAuto').isChecked()&&await page.locator('#internetTarget').inputValue()==='0','saved form follows external state');
   await page.locator('#internetAuto').uncheck();reject=true;
   await page.locator('#saveInternet').click();await idle();await page.evaluate(()=>refresh());await idle();
   check(await page.evaluate(()=>internetDraft)&&!await page.locator('#internetAuto').isChecked(),'rejected save preserves draft');
   reject=false;status={...status,boot_id:102};await page.evaluate(()=>refresh());await idle();
   check(!await page.evaluate(()=>internetDraft)&&await page.locator('#internetAuto').isChecked(),'reboot reloads saved preferences');
   for(const [expected,steps] of [['port',[-111,5000,5000]],['auth',[0,2002,5000]],['udp',[0,0,-110]],['dns',[0,0,4003]]]){
    await apply({job_action:4,job_id:200+checks,internet_checked_ms:0,internet_state:0,
     probe:steps.map(code=>({code,ms:1}))});
    check(await kind()===expected,'diagnostic advice identifies '+expected);
    check(await page.locator('#diagnosticsDetails').evaluate(node=>node.open),'failed result opens diagnostics');
    check((await page.locator('#nextHelp').textContent()).length>30,'failure includes actionable help');
   }
   await apply({internet_state:3,internet_probe:[0,0,0,6200].map(code=>({code,ms:1}))});
   check(await kind()==='target','HTTP mismatch is a target failure');
   await apply({internet_state:4,internet_probe:[-110,5000,5000,5000].map(code=>({code,ms:1}))});
   check(await kind()!=='port','expired check does not create a current port failure');
   await apply({sta_connected:false,sta_ip:'0.0.0.0',forwarding_ready:false});
   check(await kind()==='wifi','upstream Wi-Fi failure has guidance');
   await apply({sta_connected:false,forwarding_ready:false},{sta_ssid:''});
   check(await kind()==='setup'&&await page.locator('#configDetails').evaluate(node=>node.open),'first setup opens basic settings');
   await page.locator('#configDetails > summary').click();await page.evaluate(()=>refresh());await idle();
   check(!await page.locator('#configDetails').evaluate(node=>node.open),'polling respects manual disclosure closure');
   await apply({subnet_overlap:true});
   check(await kind()==='subnet'&&await page.locator('#advancedSettings').evaluate(node=>node.open),'overlap opens subnet settings');
   await apply({pending:true,confirm_seconds:120,internet_checked_ms:0,internet_state:0});
   check(await page.locator('#trial').isVisible()&&await kind()==='trial','confirmation remains prominent');
   await apply();
   await page.locator('#configDetails').evaluate(node=>node.open=true);
   await page.locator('#advancedSettings').evaluate(node=>node.open=true);
   await page.locator('input[name="dns_ipv4"]').fill('');
   await page.locator('#advancedSettings > summary').click();
   const mutations=posts.length;await page.locator('#settings button[type="submit"]').click();
   check(await page.locator('#advancedSettings').evaluate(node=>node.open),'native invalid field opens advanced settings');
   check(posts.length===mutations,'invalid form does not POST');
   await page.locator('input[name="dns_ipv4"]').fill('1.1.1.1');
   await apply();
   await page.locator('#configDetails').evaluate(node=>node.open=true);
   await page.locator('#advancedSettings').evaluate(node=>node.open=true);
   const beforeUTF8=posts.length;
   await page.locator('input[name="admin_password"]').fill('管理测');
   await page.locator('#settings button[type="submit"]').click();
   check(posts.length===beforeUTF8&&(await page.locator('#message').textContent()).includes('UTF-8'),'9-byte management password has a localized byte error');
   await page.locator('input[name="admin_password"]').fill('中'.repeat(22));
   await page.locator('#settings button[type="submit"]').click();
   check(posts.length===beforeUTF8,'66-byte management password never reaches the API');
   await page.locator('input[name="admin_password"]').fill('管理测试');
   await page.locator('input[name="ap_ssid"]').fill('中'.repeat(11));
   await page.locator('#settings button[type="submit"]').click();
   check(posts.length===beforeUTF8,'33-byte SSID never reaches the API');
   await page.locator('input[name="ap_ssid"]').fill('中'.repeat(10)+'ab');
   await page.locator('input[name="ap_password"]').fill('中文ab');
   await page.locator('#settings button[type="submit"]').click();await idle();
   check(posts.length===beforeUTF8+1&&posts.at(-1).payload.admin_password==='管理测试'&&posts.at(-1).payload.ap_password==='中文ab','valid UTF-8 passwords submit through the real form');
   check(await page.evaluate(()=>operationView().blocked),'network changes retain the restart wait');
   await apply({boot_id:status.boot_id+1});
   await apply({pending:true,confirm_seconds:120});
   await page.locator('#statusDetails').evaluate(node=>node.open=true);
   const beforeLost=posts.length;dropImmediate='unapplied';
   await page.locator('#ledEnabled').uncheck();await idle();
   check(!await page.locator('#confirm').isDisabled(),'lost LED response leaves configuration confirmation enabled');
   await page.evaluate(()=>refresh());await idle();
   check(posts.length===beforeLost+1,'status reads never replay a lost instant POST');
   dropImmediate='';await page.locator('#confirm').click();await idle();
   check(!status.pending&&posts.at(-1).path==='/api/confirm','confirmation works before the old 90-second wait');
   status={...status,led_enabled:true};await page.evaluate(()=>refresh());await idle();dropImmediate='applied';
   await page.locator('#ledEnabled').uncheck();await idle();
   check(!await page.evaluate(()=>operationView().blocked)&&(await page.locator('#operation').textContent()).includes(locale==='zh-CN'?'已达到':'now visible'),'observed LED target resolves a lost response');
   await page.locator('#diagnosticsDetails').evaluate(node=>node.open=true);
   await page.locator('#internetAuto').uncheck();await page.locator('#saveInternet').click();await idle();
   check(!await page.evaluate(()=>internetDraft),'observed saved check settings clear the unsaved draft');
   dropImmediate='';
   for(const id of ['configDetails','diagnosticsDetails','statusDetails','maintenanceDetails'])
    await page.locator('#'+id).evaluate(node=>node.open=false);
   check(errors.length===0,'no uncaught browser errors: '+errors.join('; '));
   check(!consoleErrors.some(error=>error.includes('not focusable')),'collapsed fields remain focusable for validation');
   await page.unroute('**/*');
   let setupPosts=[];
   await page.route('**/*',route=>{
    if(route.request().method()==='POST'){setupPosts.push(route.request().postDataJSON());return route.fulfill({contentType:'application/json',body:'{"configured":true}'});}
    return route.fulfill({contentType:'text/html',body:setup});
   });
   await page.reload();
   check(await page.locator('html').getAttribute('lang')===(locale==='zh-CN'?'zh-CN':'en'),'first-use language preserved');
   for(const password of ['管理测','中'.repeat(22)]){
    await page.locator('#password').fill(password);await page.locator('#confirm').fill(password);
    await page.locator('#save').click();
    check(setupPosts.length===0,'first-use page rejects out-of-range UTF-8 byte lengths');
   }
   await page.locator('#password').fill('管理测试');await page.locator('#confirm').fill('管理测试');
   await page.locator('#save').click();await page.locator('#login').waitFor({state:'visible'});
   check(setupPosts.length===1&&setupPosts[0].password==='管理测试','first-use page accepts a 12-byte / four-character password');
   check(errors.length===0,'first-use page has no uncaught error');
   console.log(`Chromium ${locale} ${width}px: ${checks} assertions PASS`);
   await context.close();
  }
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
