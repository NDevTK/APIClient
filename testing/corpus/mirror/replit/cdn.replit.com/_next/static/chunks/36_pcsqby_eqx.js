;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="f644aece-e850-a7e9-153e-62da8dc630c3")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,522624,e=>{"use strict";e.s(["urlAlphabet",0,"useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict"])},179104,e=>{"use strict";e.i(522624),e.s(["nanoid",0,(e=21)=>crypto.getRandomValues(new Uint8Array(e)).reduce((e,t)=>((t&=63)<36?e+=t.toString(36):t<62?e+=(t-26).toString(36).toUpperCase():t>62?e+="-":e+="_",e),"")])},513484,(e,t,i)=>{!function(){"use strict";var e={922:function(e){e.exports=function(e,i,a,o){i=i||"&",a=a||"=";var n={};if("string"!=typeof e||0===e.length)return n;var r=/\+/g;e=e.split(i);var s=1e3;o&&"number"==typeof o.maxKeys&&(s=o.maxKeys);var l=e.length;s>0&&l>s&&(l=s);for(var c=0;c<l;++c){var u,d,m,f,p=e[c].replace(r,"%20"),g=p.indexOf(a);(g>=0?(u=p.substr(0,g),d=p.substr(g+1)):(u=p,d=""),m=decodeURIComponent(u),f=decodeURIComponent(d),Object.prototype.hasOwnProperty.call(n,m))?t(n[m])?n[m].push(f):n[m]=[n[m],f]:n[m]=f}return n};var t=Array.isArray||function(e){return"[object Array]"===Object.prototype.toString.call(e)}},790:function(e){var t=function(e){switch(typeof e){case"string":return e;case"boolean":return e?"true":"false";case"number":return isFinite(e)?e:"";default:return""}};e.exports=function(e,n,r,s){return(n=n||"&",r=r||"=",null===e&&(e=void 0),"object"==typeof e)?a(o(e),function(o){var s=encodeURIComponent(t(o))+r;return i(e[o])?a(e[o],function(e){return s+encodeURIComponent(t(e))}).join(n):s+encodeURIComponent(t(e[o]))}).join(n):s?encodeURIComponent(t(s))+r+encodeURIComponent(t(e)):""};var i=Array.isArray||function(e){return"[object Array]"===Object.prototype.toString.call(e)};function a(e,t){if(e.map)return e.map(t);for(var i=[],a=0;a<e.length;a++)i.push(t(e[a],a));return i}var o=Object.keys||function(e){var t=[];for(var i in e)Object.prototype.hasOwnProperty.call(e,i)&&t.push(i);return t}}},i={};function a(t){var o=i[t];if(void 0!==o)return o.exports;var n=i[t]={exports:{}},r=!0;try{e[t](n,n.exports,a),r=!1}finally{r&&delete i[t]}return n.exports}a.ab="/ROOT/node_modules/.pnpm/next@16.3.0_@babel+core@7.29.7_@opentelemetry+api@1.9.0_@types+node@22.18.6_babel-plugi_beaf1314979e495ed83ef8a5eac08a13/node_modules/next/dist/compiled/querystring-es3/";var o={};o.decode=o.parse=a(922),o.encode=o.stringify=a(790),t.exports=o}()},229952,(e,t,i)=>{var a={221:function(t){"use strict";t.exports=e.r(513484)}},o={};function n(e){var t=o[e];if(void 0!==t)return t.exports;var i=o[e]={exports:{}},r=!0;try{a[e](i,i.exports,n),r=!1}finally{r&&delete o[e]}return i.exports}n.ab="/ROOT/node_modules/.pnpm/next@16.3.0_@babel+core@7.29.7_@opentelemetry+api@1.9.0_@types+node@22.18.6_babel-plugi_beaf1314979e495ed83ef8a5eac08a13/node_modules/next/dist/compiled/native-url/";var r={};!function(){var e,t=(e=n(221))&&"object"==typeof e&&"default"in e?e.default:e,i=/https?|ftp|gopher|file/;function a(e){"string"==typeof e&&(e=v(e));var a,o,n,r,s,l,c,u,d,m=(o=(a=e).auth,n=a.hostname,r=a.protocol||"",s=a.pathname||"",l=a.hash||"",c=a.query||"",u=!1,o=o?encodeURIComponent(o).replace(/%3A/i,":")+"@":"",a.host?u=o+a.host:n&&(u=o+(~n.indexOf(":")?"["+n+"]":n),a.port&&(u+=":"+a.port)),c&&"object"==typeof c&&(c=t.encode(c)),d=a.search||c&&"?"+c||"",r&&":"!==r.substr(-1)&&(r+=":"),a.slashes||(!r||i.test(r))&&!1!==u?(u="//"+(u||""),s&&"/"!==s[0]&&(s="/"+s)):u||(u=""),l&&"#"!==l[0]&&(l="#"+l),d&&"?"!==d[0]&&(d="?"+d),{protocol:r,host:u,pathname:s=s.replace(/[?#]/g,encodeURIComponent),search:d=d.replace("#","%23"),hash:l});return""+m.protocol+m.host+m.pathname+m.search+m.hash}var o="http://",s=o+"w.w",l=/^([a-z0-9.+-]*:\/\/\/)([a-z0-9.+-]:\/*)?/i,c=/https?|ftp|gopher|file/;function u(e,t){var i="string"==typeof e?v(e):e;e="object"==typeof e?a(e):e;var n=v(t),r="";i.protocol&&!i.slashes&&(r=i.protocol,e=e.replace(i.protocol,""),r+="/"===t[0]||"/"===e[0]?"/":""),r&&n.protocol&&(r="",n.slashes||(r=n.protocol,t=t.replace(n.protocol,"")));var u=e.match(l);u&&!n.protocol&&(e=e.substr((r=u[1]+(u[2]||"")).length),/^\/\/[^/]/.test(t)&&(r=r.slice(0,-1)));var d=new URL(e,s+"/"),m=new URL(t,d).toString().replace(s,""),f=n.protocol||i.protocol;return f+=i.slashes||n.slashes?"//":"",!r&&f?m=m.replace(o,f):r&&(m=m.replace(o,"")),c.test(m)||~t.indexOf(".")||"/"===e.slice(-1)||"/"===t.slice(-1)||"/"!==m.slice(-1)||(m=m.slice(0,-1)),r&&(m=r+("/"===m[0]?m.substr(1):m)),m}function d(){}d.prototype.parse=v,d.prototype.format=a,d.prototype.resolve=u,d.prototype.resolveObject=u;var m=/^https?|ftp|gopher|file/,f=/^(.*?)([#?].*)/,p=/^([a-z0-9.+-]*:)(\/{0,3})(.*)/i,g=/^([a-z0-9.+-]*:)?\/\/\/*/i,h=/^([a-z0-9.+-]*:)(\/{0,2})\[(.*)\]$/i;function v(e,i,o){if(void 0===i&&(i=!1),void 0===o&&(o=!1),e&&"object"==typeof e&&e instanceof d)return e;var n=(e=e.trim()).match(f);e=n?n[1].replace(/\\/g,"/")+n[2]:e.replace(/\\/g,"/"),h.test(e)&&"/"!==e.slice(-1)&&(e+="/");var r=!/(^javascript)/.test(e)&&e.match(p),l=g.test(e),c="";r&&(m.test(r[1])||(c=r[1].toLowerCase(),e=""+r[2]+r[3]),r[2]||(l=!1,m.test(r[1])?(c=r[1],e=""+r[3]):e="//"+r[3]),3!==r[2].length&&1!==r[2].length||(c=r[1],e="/"+r[3]));var u,v=(n?n[1]:e).match(/^https?:\/\/[^/]+(:[0-9]+)(?=\/|$)/),x=v&&v[1],y=new d,_="",w="";try{u=new URL(e)}catch(t){_=t,c||o||!/^\/\//.test(e)||/^\/\/.+[@.]/.test(e)||(w="/",e=e.substr(1));try{u=new URL(e,s)}catch(e){return y.protocol=c,y.href=c,y}}y.slashes=l&&!w,y.host="w.w"===u.host?"":u.host,y.hostname="w.w"===u.hostname?"":u.hostname.replace(/(\[|\])/g,""),y.protocol=_?c||null:u.protocol,y.search=u.search.replace(/\\/g,"%5C"),y.hash=u.hash.replace(/\\/g,"%5C");var b=e.split("#");!y.search&&~b[0].indexOf("?")&&(y.search="?"),y.hash||""!==b[1]||(y.hash="#"),y.query=i?t.decode(u.search.substr(1)):y.search.substr(1),y.pathname=w+(r?u.pathname.replace(/['^|`]/g,function(e){return"%"+e.charCodeAt().toString(16).toUpperCase()}).replace(/((?:%[0-9A-F]{2})+)/g,function(e,t){try{return decodeURIComponent(t).split("").map(function(e){var t=e.charCodeAt();return t>256||/^[a-z0-9]$/i.test(e)?e:"%"+t.toString(16).toUpperCase()}).join("")}catch(e){return t}}):u.pathname),"about:"===y.protocol&&"blank"===y.pathname&&(y.protocol="",y.pathname=""),_&&"/"!==e[0]&&(y.pathname=y.pathname.substr(1)),c&&!m.test(c)&&"/"!==e.slice(-1)&&"/"===y.pathname&&(y.pathname=""),y.path=y.pathname+y.search,y.auth=[u.username,u.password].map(decodeURIComponent).filter(Boolean).join(":"),y.port=u.port,x&&!y.host.endsWith(x)&&(y.host+=x,y.port=x.slice(1)),y.href=w?""+y.pathname+y.search+y.hash:a(y);var C=/^(file)/.test(y.href)?["host","hostname"]:[];return Object.keys(y).forEach(function(e){~C.indexOf(e)||(y[e]=y[e]||null)}),y}r.parse=v,r.format=a,r.resolve=u,r.resolveObject=function(e,t){return v(u(e,t))},r.Url=d}(),t.exports=r},810047,(e,t,i)=>{t.exports=e.r(362270)},864891,e=>{"use strict";var t=e.i(389959),i=e.i(830675),a=e.i(569910),o=e.i(694387),n=e.i(779664),r=e.i(443197),s=e.i(625006),l=e.i(320216),c=e.i(489859);let u="pendingConnectedAccountLink";e.s(["default",0,function(){let e=(0,t.useRef)(!1),d=(0,s.useSaveUserAuth)(),{showMessage:m}=(0,l.default)();return(0,t.useEffect)(()=>{e.current||!0!==c.default.get(u,"boolean")||(e.current=!0,c.default.remove(u),(0,r.handlePotentialOAuthRedirect)().then(async e=>{let t;if("success"===e.type){if(!e.result)return;if(e.result.providerId===o.GitHubProviderId){try{let t=await (0,n.setCookie)(e.result.user,{intent:"provider_link"});if("error"===t.type)throw Error(t.message)}catch(e){i.captureException(e);return}let t=await d({userCredential:e.result,oauthProviderId:o.GitHubProviderId,scopes:[]});if("error"===t.type)return void m({content:t.message,type:"error"})}m({content:"Account connected successfully.",type:"confirm"});return}switch(e.treatment){case"credential-conflict":t="This provider cannot be used, please try another method.";break;case"email-conflict":t="This email cannot be used, please try another method.";break;case"forbidden-operation":t="That action is not permitted.";break;case"user-needs-email":t="Adding this provider failed because it doesn't have an associated email address.";break;case"requires-provider-link":case"timeout":case"unhandled":case"unknown":t="Something unexpected happened while connecting the account. Please try again.";break;default:(0,a.default)(e)}m({content:t,type:"error"})}).catch(e=>i.captureException(e)))},[d,m]),null},"setPendingConnectedAccountLink",0,function(){c.default.set(u,!0)}])},224629,e=>{"use strict";var t=e.i(779664),i=e.i(429843);let a=new class{config;activeRefreshPromise=null;constructor(e){this.config={maxRetries:3,baseDelayMs:1e3,maxDelayMs:5e3,...e}}async refreshCookie(e){return this.activeRefreshPromise?i.logger.info("Cookie refresh already in progress, waiting for existing operation to complete"):this.activeRefreshPromise=this.executeRefreshOperation(e),this.activeRefreshPromise}async executeRefreshOperation(e){let t=null;try{for(let a=1;a<=this.config.maxRetries;a++){if(t=await this.attemptCookieRefresh(e,a),"success"===t.type)return a>1&&i.logger.info("Cookie refresh succeeded after retries",{attempt:a,totalAttempts:a}),t;if("redirecting"===t.type)return t;if(!this.isRetryableError(t))return i.logger.warn("Cookie refresh failed with non-retryable error",{attempt:a,resultType:t.type,errorMessage:t.message}),t;if(i.logger.warn("Cookie refresh attempt failed, will retry",{attempt:a,resultType:t.type,errorMessage:t.message,remainingAttempts:this.config.maxRetries-a}),a<this.config.maxRetries){let e=this.calculateBackoffDelay(a);await new Promise(t=>setTimeout(t,e))}}return i.logger.error("Cookie refresh failed after all retry attempts",{finalAttempt:this.config.maxRetries,lastError:t?.type,lastMessage:t?.message}),t||{type:"error",message:"Cookie refresh failed after all retry attempts"}}finally{this.activeRefreshPromise=null}}async attemptCookieRefresh(e,a){try{return i.logger.info("Cookie refresh attempt",{attempt:a,maxRetries:this.config.maxRetries}),await (0,t.setCookie)(e)}catch(e){return i.logger.warn("Cookie refresh attempt failed with exception",{attempt:a,errorDetails:e instanceof Error?e.message:String(e)}),{type:"error",message:e instanceof Error?e.message:"Unknown error during cookie refresh"}}}isRetryableError(e){return"needs_email_verification"!==e.type&&("error"!==e.type||void 0===e.reason)&&"error"===e.type}calculateBackoffDelay(e){let t=Math.min(this.config.baseDelayMs*Math.pow(2,e-1),this.config.maxDelayMs),i=.25*t*(Math.random()-.5);return Math.round(t+i)}getState(){return{hasActivePromise:null!==this.activeRefreshPromise}}reset(){this.activeRefreshPromise=null}};e.s(["cookieRefreshService",0,a])},594997,e=>{"use strict";var t=e.i(276385),i=e.i(389959),a=e.i(830675),o=e.i(408116),n=e.i(224629),r=e.i(706985),s=e.i(697373),l=e.i(443197),c=e.i(429843),u=e.i(753451);let d=(0,i.createContext)({user:void 0});e.s(["default",0,({children:e})=>{let m=(0,o.useApolloClient)(),[f,p]=(0,i.useState)(),g=(0,u.useIsInBonsaiWebview)();return(0,i.useEffect)(()=>{let e=null,t=(0,l.onTokenUpdate)(async e=>{if(e?p(e):p(void 0),e&&await (0,l.shouldRefreshCookie)(e)){let t=await n.cookieRefreshService.refreshCookie(e);if("success"===t.type)return void c.logger.info("Cookie refresh succeeded");if("error"===t.type&&void 0!==t.reason){await (0,l.firebaseSignOut)(),(0,s.redirectToLoginWithFreshSession)();return}c.logger.warn("Cookie refresh failed after all retries, signing out user"),await (0,l.signOut)(m)}});(0,r.maybeBootstrapDevAutoLogin)();let i=async()=>{try{let e=(0,l.getAuthInstance)().currentUser;e&&!e.isAnonymous&&await e.getIdToken(!0)}catch(e){a.withScope(t=>{t.setExtra("proactiveTokenRefresh",!0),t.setExtra("isInBonsaiWebview",g),a.captureException(e)})}};return g||(e=setInterval(i,15e5)),()=>(e&&clearInterval(e),t())},[m,g]),(0,t.jsxs)(d.Provider,{value:{user:f},children:[e,(0,t.jsx)("style",{"data-replit-global":"token-context-grecaptcha",children:".grecaptcha-badge{visibility:hidden}"})]})},"useTokenContext",0,()=>(0,i.useContext)(d)])},706985,e=>{"use strict";e.i(830675),e.i(779664),e.i(443197),e.s(["maybeBootstrapDevAutoLogin",0,()=>Promise.resolve()])},672259,e=>{"use strict";var t=e.i(694387),i=e.i(784398),a=e.i(331820),o=e.i(886527),n=e.i(562821),r=e.i(927976);e.s(["mapRedirectErrorMessage",0,function({redirect:e,mode:i,intl:a}){switch(e.treatment){case"requires-provider-link":return a.formatMessage({id:"auth.errorAccountExistsLinkProvider",defaultMessage:"Please log in to {email} with another method and link {providerName} from the account page."},{email:e.context.email,providerName:(0,t.getOAuthProviderName)(e.context.credential.providerId)});case"user-needs-email":return a.formatMessage({id:"auth.errorProviderMissingEmail",defaultMessage:"Your {providerName} account is missing an email address. To use it for Replit, you'll need to {action} with another method and connect {providerName} from the account page."},{providerName:(0,t.getOAuthProviderName)(e.credential.providerId),action:"login"===i?a.formatMessage({id:"auth.actionLogIn",defaultMessage:"log in"}):a.formatMessage({id:"auth.actionSignUp",defaultMessage:"sign up"})});case"credential-conflict":return a.formatMessage({id:"auth.errorProviderCannotBeUsed",defaultMessage:"This provider cannot be used, please try another method."});case"email-conflict":return a.formatMessage({id:"auth.errorEmailCannotBeUsed",defaultMessage:"This email cannot be used, please try another method."});case"forbidden-operation":return a.formatMessage({id:"auth.errorForbiddenOperation",defaultMessage:"That action is not permitted"});default:return a.formatMessage({id:"auth.errorSomethingUnexpectedTryAgain",defaultMessage:"Something unexpected happened, please try again."})}},"resolvePostAuthRedirectUrl",0,function({authedRedirectUrl:e,postLoginRedirectUrl:t,postAuthGoto:s,stack:l}){if((0,r.getLoggedOutExperiencePrompt)())return(0,i.loggedOutExperienceRedirectUrl)((0,r.getLoggedOutExperiencePrompt)()?.utmCampaign);if(s)return(0,o.default)(window.location,s);if(t)return t;if(e)return(0,a.default)({redirectUrl:e});let c=(0,n.default)().as;if(!l)return c;let u=c.includes("?")?"&":"?";return`${c}${u}stack=${encodeURIComponent(l)}`}])},789414,e=>{"use strict";var t=e.i(15801),i=e.i(389959),a=e.i(779664),o=e.i(784398),n=e.i(845734),r=e.i(624695),s=e.i(672259),l=e.i(443197);e.i(214847);var c=e.i(864300),u=e.i(905339),d=e.i(917248),m=e.i(927976),f=e.i(776065),p=e.i(415541),g=e.i(709485);e.s(["useOAuthRedirectHandler",0,function(){let e=(0,t.useRouter)(),h=(0,c.useIntl)(),[v,x]=(0,i.useState)(!1),[y,_]=(0,i.useState)(null),[w,b]=(0,i.useState)(null),C=(0,i.useRef)(!1),N=(0,i.useRef)(h);N.current=h;let[M,I]=(0,i.useState)("signup"),[j,R]=(0,i.useState)(),T=(0,i.useCallback)(()=>{let{searchParams:t}=new URL(e.asPath,"http://localhost");t.has(o.redirectingParam)&&(0,f.updatePathWithQueryParams)({router:e,params:[{mode:"delete",key:o.redirectingParam}]})},[e]),S=(0,i.useCallback)(e=>{let t=N.current;x(!0),b(t.formatMessage({id:"auth.samlSsoRequiredRedirecting",defaultMessage:"SAML SSO required. Redirecting to the SSO login page..."})),setTimeout(async()=>{"error"===(await (0,l.signInWithProvider)({tenantId:e.tenantId,provider:{type:"saml",providerId:e.samlProviderId}})).type&&(_(t.formatMessage({id:"auth.errorSomethingWentWrongTryLogin",defaultMessage:"Something went wrong. Please try logging in again."})),x(!1),b(null),T())},3e3)},[T]);return(0,i.useEffect)(()=>{if(C.current)return;C.current=!0;let t=N.current,{searchParams:i}=new URL(e.asPath,"http://localhost"),c="login"===i.get(o.authModalParam)?"login":"signup",f=i.get("postAuthGoto")??void 0;I(c),R(f);let h=i.has(o.redirectingParam),v=(0,n.getPendingOAuthContext)(),y=e=>({provider:v?.provider??(e?(0,r.oauthProviderName)(e):void 0),mode:c,flow:"redirect",surface:v?.surface??"auth_form"}),w=!1,b=async(e,t)=>{x(!1),_(e),T(),t&&await (0,l.firebaseSignOut)()},M=async e=>{let t=(0,n.getPendingLoginMethod)();t?((0,n.saveLastLoginMethod)(t),(0,n.clearPendingLoginMethod)()):e.providerId&&(0,n.isValidLoginMethod)(e.providerId)&&(0,n.saveLastLoginMethod)(e.providerId);let i=(0,m.getStackPreselection)()?.stack;(0,m.clearStackPreselection)(),e.isNewUser&&((0,u.pushAdsUserData)(e.email),await (0,d.callDeviceFingerprinting)({userId:e.userId.toString(),flow:"oauth_redirect"})),window.location.href=(0,s.resolvePostAuthRedirectUrl)({authedRedirectUrl:e.redirectUrl,postAuthGoto:f,stack:i})};(0,l.handlePotentialOAuthRedirect)().then(async i=>{let o;if("success"!==i.type){let e;"requires-provider-link"===i.treatment?e=i.context.credential.providerId??void 0:"user-needs-email"===i.treatment&&(e=i.credential.providerId??void 0);let a=v??y(e);w=!0,(0,r.trackOAuthRedirectOutcome)(e,a,{stage:"provider_result",status:"failed",error_treatment:(0,r.mapOAuthErrorTreatment)(i.treatment)}),(0,n.clearPendingOAuthContext)(),(0,p.track)(g.events.SIGNUP_OAUTH_REDIRECT_RESULT,{result:"error",errorTreatment:i.treatment,providerId:e,hadRedirectingParam:h,surface:"marketing_redirect_handler"}),await b((0,s.mapRedirectErrorMessage)({redirect:i,mode:c,intl:t}),"user-needs-email"===i.treatment);return}if(null===i.result){h&&(w=!0,(0,r.trackOAuthOutcome)(v??y(void 0),{stage:"provider_result",status:"no_result"}),(0,p.track)(g.events.SIGNUP_OAUTH_REDIRECT_RESULT,{result:"null",hadRedirectingParam:!0,surface:"marketing_redirect_handler"})),(0,n.clearPendingOAuthContext)(),x(!1),T();return}x(!0);let l=i.result.user.providerData[0]?.providerId,u=i.result.providerId??l;w=!0;let d=v??y(u);(0,r.trackOAuthRedirectOutcome)(u,d,{stage:"provider_result",status:"success"});try{o=await (0,a.setCookie)(i.result.user,{source:"/iab_campaign"===e.pathname?"iab_campaign":e.pathname})}catch(e){throw(0,r.trackOAuthRedirectOutcome)(u,d,{stage:"session_exchange",status:"failed",error_treatment:"session_exchange_failed"}),(0,n.clearPendingOAuthContext)(),e}if("redirecting"===o.type)return void(0,n.clearPendingOAuthContext)();if((0,p.track)(g.events.SIGNUP_OAUTH_REDIRECT_RESULT,{result:"success",providerId:i.result.providerId??void 0,isNewUser:"success"===o.type?o.isNewUser:void 0,hadRedirectingParam:h,surface:"marketing_redirect_handler"}),"success"===o.type){(0,r.trackOAuthRedirectOutcome)(u,d,{stage:"session_exchange",status:"success",user_outcome:o.isNewUser?"new_user":"existing_user"}),(0,n.clearPendingOAuthContext)(),await M({...o,providerId:l,email:i.result.user.email});return}if(o.reauth){(0,r.trackOAuthRedirectOutcome)(u,d,{stage:"session_exchange",status:"failed",error_treatment:"session_exchange_failed"}),(0,n.clearPendingOAuthContext)(),l&&(0,n.isValidLoginMethod)(l)&&(0,n.savePendingLoginMethod)(l),S(o.reauth);return}(0,r.trackOAuthRedirectOutcome)(u,d,{stage:"session_exchange",status:"failed",error_treatment:"needs_email_verification"===o.type?"user_needs_email":"session_exchange_failed"}),(0,n.clearPendingOAuthContext)(),await b(o.message,!0)}).catch(()=>(w||(0,r.trackOAuthOutcome)(v??y(void 0),{stage:"provider_result",status:"failed",error_treatment:"unhandled"}),(0,n.clearPendingOAuthContext)(),b(t.formatMessage({id:"auth.errorSomethingWentWrongTryAgain",defaultMessage:"Something went wrong, please try again."}),!1)))},[e,S,T]),{isRedirecting:v,error:y,samlRedirectMessage:w,mode:M,postAuthGoto:j,authWithSamlTenant:S,setIsRedirecting:x,setError:_}}])},924840,e=>{"use strict";var t=e.i(276385),i=e.i(908796),a=e.i(596139),o=e.i(884214),n=e.i(615982),r=e.i(955410);e.i(214847);var s=e.i(614852),l=e.i(864300),c=e.i(242917),u=e.i(643484),d=e.i(108431),m=e.i(124298);function f(e){if(!e||e.status!==i.DowngradeStatus.Scheduled||new Date(e.scheduledFor).getTime()<=Date.now())return null;let t=(0,a.getCheckoutablePriceByExternalId)(e.targetPriceId);if(!t)return null;let o=(0,s.formatDate)(e.scheduledFor,{preset:"medium"});if(t.planPrefix===a.proPlanPrefix){var n;let i,r=(n=e.fromPriceId,null==(i=(0,a.getCheckoutablePriceByExternalId)(n)?.price.allocation.amount)?null:(0,s.formatCurrency)(i,{maximumFractionDigits:0}));return r?{kind:"allocation",fromAllocation:r,toAllocation:(0,s.formatCurrency)(t.price.allocation.amount,{maximumFractionDigits:0}),formattedDate:o}:null}return{kind:"plan",planName:a.individualPlanNameFromPrefix[t.planPrefix],formattedDate:o}}e.s(["ScheduledDowngradeBanner",0,function({scheduledDowngrade:e,variant:i,onDismiss:a}){let s,p=(0,l.useIntl)(),{show:g}=(0,c.useGlobalModal)(),{trackClick:h}=(0,r.useTrackClick)(),v=(0,o.useAutoLogView)({elementId:"home_scheduled_downgrade_banner"}),x=(0,o.useAutoLogView)({elementId:"billing_scheduled_downgrade_banner"}),y=f(e);if(!y)return null;s="allocation"===y.kind?p.formatMessage({id:"billing.scheduledDowngradeBannerAllocation",defaultMessage:"Your monthly usage credits are scheduled to decrease from {fromAllocation} to {toAllocation} on {date}."},{fromAllocation:y.fromAllocation,toAllocation:y.toAllocation,date:y.formattedDate}):p.formatMessage({id:"billing.scheduledDowngradeBanner",defaultMessage:"Your plan is scheduled to downgrade to {planName} on {date}."},{planName:y.planName,date:y.formattedDate});let _="top"===i?"home_page_scheduled_downgrade_banner_cancel_button":"billing_page_scheduled_downgrade_banner_cancel_button",w="top"===i?"home_banner":"billing_settings",b=e?.canCancel?(0,t.jsx)(u.Button,{"data-analytics-id":"top"===i?"home_scheduled_downgrade_banner_cancel_button":"billing_scheduled_downgrade_banner_cancel_button",variant:"underlined",size:"small",text:p.formatMessage({id:"billing.cancelDowngrade",defaultMessage:"Cancel downgrade"}),onClick:()=>{h({productArea:"billing",target:_}),g("CancelScheduledDowngradeModal",{customerId:e.customerId,targetPriceId:e.targetPriceId,scheduledFor:e.scheduledFor,isWithinPro:"allocation"===y.kind,subscriptionPlanChangeFlow:(0,n.startSubscriptionPlanChangeFlow)(w,{flow_type:"plan_change_cancellation_flow",plan_change_kind:"cancel_scheduled_downgrade",element:"cancel_scheduled_downgrade_button",flow_step_key:"started",interaction_status:"success",surface:"scheduled_downgrade_banner"})})}}):null;return"top"===i?(0,t.jsx)(m.TopBanner,{dataAnalyticsId:"home_scheduled_downgrade_banner",dismissButtonAnalyticsId:"home_scheduled_downgrade_banner_dismiss_button",colorway:"themeNotice",text:s,onDismiss:a,innerRef:v,children:b}):(0,t.jsx)(d.StatusBanner,{dataAnalyticsId:"billing_scheduled_downgrade_banner",dismissButtonAnalyticsId:"billing_scheduled_downgrade_banner_dismiss_button",colorway:"blue",closable:!!a,closeAction:a,text:s,action:b??void 0,innerRef:x})},"getScheduledDowngradeBannerContent",0,f])},124054,e=>{"use strict";var t=e.i(912206),i=e.i(466366),a=e.i(709485),o=e.i(415541),n=e.i(919246);function r(e){return{entryPoint:e,platform:(0,n.getWebTrackPlatform)({userAgent:window.navigator.userAgent}),handle:(0,i.startSubscriptionFlow)(t.v4,t.v4)}}function s(e,t){return{attempt_id:e.handle.attemptId,entry_point:e.entryPoint,flow_id:e.handle.flowId,flow_step_index:t,logging_type:"client",logging_version:2}}e.s(["appendPortalCancelParams",0,function(e,t){let[i,a]=e.split("?"),o=new URLSearchParams(a);for(let[e,i]of new URLSearchParams({cancel_intent:"1",attempt_id:t.handle.attemptId,entry_point:t.entryPoint,flow_id:t.handle.flowId,platform:t.platform}))o.set(e,i);return`${i}?${o.toString()}`},"startSubscriptionCancellationFlow",0,function(e,t){let n=r(e),l=(0,i.recordSubscriptionStep)(n.handle);return n.handle=l.flow,l.shouldEmit&&(0,o.trackV2)(a.eventsV2.SUBSCRIPTION_CLICKED,{...s(n,l.flowStepIndex),flow_type:"cancellation_flow",surface:t.surface,element:t.element,flow_step_key:"started",interaction_status:"success"}),n},"trackSubscriptionCancellationNavClick",0,function(e,t){let n=(0,i.recordSubscriptionStep)(e.handle);e.handle=n.flow,n.shouldEmit&&(0,o.trackV2)(a.eventsV2.SUBSCRIPTION_CLICKED,{...s(e,n.flowStepIndex),flow_type:"cancellation_flow",surface:t.surface,element:t.element,flow_step_key:"confirmation_viewed",interaction_status:"success"})},"trackSubscriptionCancellationSubmit",0,function(e,t){let n=(0,i.recordSubscriptionStep)(e.handle);if(e.handle=n.flow,!n.shouldEmit)return;(0,o.trackV2)(a.eventsV2.SUBSCRIPTION_CLICKED,{...s(e,n.flowStepIndex),flow_type:"cancellation_flow",surface:t.surface,element:t.element,flow_step_key:"submitted",interaction_status:"success"});let r=(0,i.recordSubscriptionStep)(e.handle);e.handle=r.flow,r.shouldEmit&&((0,o.trackV2)(a.eventsV2.SUBSCRIPTION_UPDATED,{...s(e,r.flowStepIndex),...void 0!==t.paymentProvider?{payment_provider:t.paymentProvider}:{},action:"cancelled",status:"pending",flow_step_key:"submitted"}),e.handle=(0,i.endSubscriptionFlow)(e.handle))},"trackSubscriptionCancellationView",0,function(e,t){let n=(0,i.recordSubscriptionView)(e.handle,{surface:t,flow_step_key:"confirmation_viewed"});e.handle=n.flow,n.shouldEmit&&(0,o.trackV2)(a.eventsV2.SUBSCRIPTION_VIEWED,{...s(e,n.flowStepIndex),flow_type:"cancellation_flow",surface:t,flow_step_key:"confirmation_viewed",interaction_status:"success"})},"trackSubscriptionRestoreClick",0,function(e){let t=r(e),n=(0,i.recordSubscriptionStep)(t.handle);t.handle=n.flow,n.shouldEmit&&(0,o.trackV2)(a.eventsV2.SUBSCRIPTION_CLICKED,{...s(t,n.flowStepIndex),flow_type:"reactivation_flow",surface:"subscription_restore",element:"restore_subscription_button",flow_step_key:"started",interaction_status:"success"})}])},238085,e=>{"use strict";e.i(214847);let t=(0,e.i(800686).defineMessages)({chargeFailedText:{id:"billing.autoReloadChargeFailedBannerText",defaultMessage:"Your auto-reload failed. We couldn't charge your saved card. Update it to restore auto-reload and avoid downtime."},updateCard:{id:"billing.autoReloadChargeFailedUpdateCard",defaultMessage:"Update card"},limitReachedText:{id:"billing.autoReloadLimitReachedBannerText",defaultMessage:"You've reached your monthly top up auto-reload limit. Increase limit or top up to keep building."},manageLimit:{id:"billing.autoReloadLimitReachedManageLimit",defaultMessage:"Manage limit"}});e.s(["autoReloadBannerMessages",0,t])},683174,e=>{"use strict";var t=e.i(351623),i=e.i(344480);e.i(975473);let a={},o=t.gql`
    fragment AutoReloadFailureStateFields on CustomerTopUpState {
  autoTopUp {
    __typename
    ... on CustomerAutoTopUpPaymentFailed {
      invoice {
        id
        hostedInvoiceUrl
      }
    }
    ... on CustomerAutoTopUpDisabledMonthlyCap {
      disabledAt
    }
  }
}
    `,n=t.gql`
    query AutoReloadTopUpEligibility {
  currentUser {
    id
    isTopUpCustomer
  }
}
    `,r=t.gql`
    query AutoReloadFailureState {
  currentUser {
    id
    customer {
      id
      topUpState {
        __typename
        ... on CustomerTopUpState {
          ...AutoReloadFailureStateFields
        }
      }
    }
  }
}
    ${o}`;e.s(["AutoReloadFailureStateFieldsFragmentDoc",0,o,"useAutoReloadFailureStateQuery",0,function(e){let t={...a,...e};return i.useQuery(r,t)},"useAutoReloadTopUpEligibilityQuery",0,function(e){let t={...a,...e};return i.useQuery(n,t)}])},750414,e=>{"use strict";var t=e.i(683174);function i(e){return{failedInvoice:e?.__typename==="CustomerAutoTopUpPaymentFailed"?{id:e.invoice.id,hostedInvoiceUrl:e.invoice.hostedInvoiceUrl??null}:null,limitReachedAt:e?.__typename==="CustomerAutoTopUpDisabledMonthlyCap"?e.disabledAt:null}}e.s(["autoReloadFailureFromAutoTopUp",0,i,"useAutoReloadFailureState",0,function({skip:e=!1}={}){let{data:a,loading:o}=(0,t.useAutoReloadTopUpEligibilityQuery)({skip:e}),n=a?.currentUser?.isTopUpCustomer??!1,{data:r,loading:s}=(0,t.useAutoReloadFailureStateQuery)({skip:e||!n}),l=r?.currentUser?.customer?.topUpState;return{isTopUpCustomer:n,loading:!e&&(o||n&&s),...i(l?.__typename==="CustomerTopUpState"?l.autoTopUp:null)}}])},903441,e=>{e.v({bar:"index-module__9dlFLG__bar",peg:"index-module__9dlFLG__peg"})},710394,e=>{"use strict";let t,i,a,o,n;var r=e.i(15801),s=e.i(903441);let l=0,c=!1,u=null,d=null,m=!1,f=0,p=0;function g(e){let t=function(){if(u)return u;let e=document.createElement("div");e.className=s.default.bar,e.setAttribute("role","progressbar"),e.setAttribute("aria-hidden","true");let t=document.createElement("div");return t.className=s.default.peg,e.appendChild(t),document.body.appendChild(e),e.offsetWidth,u=e,e}();t.style.transition="transform 200ms ease",t.style.transform=`translate3d(${(e-1)*100}%, 0, 0)`}function h(){var e;if(null==d)return;let t=(e=d+.02*Math.random())<0?0:e>.994?.994:e;d=t,g(t),i=setTimeout(h,800)}function v(e){null!=e&&clearTimeout(e)}function x(){u?.parentNode&&u.parentNode.removeChild(u),u=null,d=null}function y(){t=v(t),i=v(i),a=v(a),o=v(o),n=v(n),m=!1,f=0,p++,x()}function _(e,t){t.shallow||0===l||(m=!0,w({useSafetyTimeout:!0}))}function w({useSafetyTimeout:e}){if(t=v(t),a=v(a),o=v(o),e&&(n=v(n),n=setTimeout(y,3e4)),null!=d&&u){u.style.opacity="1",d>=.994&&g(d=.974),i=v(i),i=setTimeout(h,800);return}t=setTimeout(()=>{t=void 0,null==d&&(d=.08,g(.08),i=setTimeout(h,800))},400)}function b(){m=!1,n=v(n),C()}function C(){var e;m||f>0||(t=v(t),n=v(n),null!=d&&(i=v(i),null!=d&&(g(d=(e=d+.3+.5*Math.random())<0?0:e>1?1:e),a=setTimeout(()=>{a=void 0,d=1,g(1),a=setTimeout(()=>{a=void 0,u&&(u.style.transition="opacity 200ms linear",u.style.opacity="0",o=setTimeout(()=>{o=void 0,x()},200))},200)},0))))}e.s(["startRouteProgress",0,function(){f++,w({useSafetyTimeout:!1});let e=!1,t=p;return()=>{e||t!==p||(e=!0,f--,C())}},"subscribeRouteProgress",0,function(){return c||(c=!0,r.default.events.on("routeChangeComplete",b),r.default.events.on("routeChangeError",b)),0===l&&r.default.events.on("routeChangeStart",_),l++,()=>{0==--l&&r.default.events.off("routeChangeStart",_)}}])},77029,e=>{"use strict";var t=e.i(490193),i=e.i(276385),a=e.i(389959),o=e.i(602351),n=e.i(19777);let r=(0,o.atom)(!1);e.s(["StrictModeWrapper",0,function({children:e}){let[o,s]=(0,n.useAtom)(r),l="1"!==t.default.env.DISABLE_STRICT_MODE;return((0,a.useEffect)(()=>{},[l,o,s]),l)?(0,i.jsx)(a.StrictMode,{children:e}):e}])},296148,e=>{"use strict";var t=e.i(208018);e.s(["default",0,function(){return(0,t.default)(()=>{function e(e){e.preventDefault()}return window.addEventListener("beforeinstallprompt",e),()=>{window.removeEventListener("beforeinstallprompt",e)}},[]),null}])},916886,e=>{"use strict";var t=e.i(389959),i=e.i(753451),a=e.i(68701);e.s(["useEnableMaxScale",0,function(){let[e,o]=(0,t.useState)(!1),n=(0,i.useIsInMobileWorkspace)(),r=(0,a.useIsIOS)(),s=(0,a.useIsAndroid)();return(0,t.useEffect)(()=>{o(r||n&&!s)},[n,s,r]),e}])},876506,e=>{"use strict";var t=e.i(276385),i=e.i(810047),a=e.i(15801),o=e.i(389959);e.i(214847);var n=e.i(864300),r=e.i(890630),s=e.i(739980),l=e.i(296148),c=e.i(916886);let u="https://replit.com/public/images/opengraph_rebrand.jpg";e.s(["default",0,({title:e,publicTitleSuffix:d,description:m,image:f=u,canonicalUrl:p,largeCard:g,viewportFitCover:h})=>{let v=(0,n.useIntl)(),x=(0,a.useRouter)(),[y,_]=(0,o.useState)(!1),w=(0,c.useEnableMaxScale)(),b=r.rebrandFavicons.prompt,C=function(e,t){if(!e)return;let i="self"===e?t.replace(/[?#].*$/,""):e;return`https://replit.com${i}`}(p,x.asPath),N=["width=device-width","initial-scale=1",w&&"maximum-scale=1",h&&"viewport-fit=cover"].filter(Boolean).join(", "),M=m??v.formatMessage({id:"home.layoutHeadDefaultDescription",defaultMessage:"Build and deploy software collaboratively with the power of AI without spending a second on setup."}),I=v.formatMessage(d?{id:"home.layoutHeadPublicTitleSuffix",defaultMessage:"{title} | Replit"}:{id:"home.layoutHeadTitleSuffix",defaultMessage:"{title} - Replit"},{title:e});return(0,o.useEffect)(()=>{window.location.host.startsWith("staging")&&_(!0)},[]),(0,t.jsxs)(t.Fragment,{children:[(0,t.jsxs)(i.default,{children:[(0,t.jsx)("title",{children:I},"title"),(0,t.jsx)("link",{rel:"shortcut icon",href:b,sizes:"192x192",type:"image/png"}),y?(0,t.jsx)("meta",{name:"robots",content:"noindex"}):null,(0,t.jsx)("meta",{property:"og:title",content:e},"meta:og:title"),(0,t.jsx)("meta",{property:"og:description",content:M},"meta:og:description"),(0,t.jsx)("meta",{property:"og:type",content:"article"},"meta:og:type"),(0,t.jsx)("meta",{property:"og:image",content:f},"meta:og:image"),(0,t.jsx)("meta",{property:"og:site_name",content:"replit"}),(0,t.jsx)("meta",{property:"fb:app_id",content:"1775481339348651"}),(0,t.jsx)("meta",{itemProp:"name",content:"replit"}),(0,t.jsx)("meta",{itemProp:"description",content:M},"meta:itemProp:description"),(0,t.jsx)("meta",{itemProp:"image",content:f},"meta:image"),(0,t.jsx)("meta",{name:"description",content:M},"meta:description"),(0,t.jsx)("meta",{name:"keywords",content:"replit,ai,software,build,collaborate,IDE,platform,code,deploy,prototype,online,agent"},"meta:keywords"),(0,t.jsx)("meta",{name:"author",property:"og:author",content:"replit"},"meta:og:author"),(0,t.jsx)("meta",{charSet:"UTF-8"}),(0,t.jsx)("meta",{name:"twitter:card",content:g?"summary_large_image":"summary"},"meta:twitter:card"),(0,t.jsx)("meta",{name:"twitter:site",content:"@replit"}),(0,t.jsx)("meta",{name:"twitter:title",content:e},"meta:twitter:title"),(0,t.jsx)("meta",{name:"twitter:description",content:M},"meta:twitter:description"),(0,t.jsx)("meta",{name:"twitter:image",content:f},"meta:twitter:image"),(0,t.jsx)("meta",{name:"twitter:domain",content:"replit.com"}),(0,t.jsx)("meta",{name:"google",content:"notranslate"}),(0,t.jsx)("meta",{name:"viewport",content:N},"viewport"),(0,t.jsx)("link",{rel:"manifest",href:"/public/manifest.json",crossOrigin:"use-credentials"}),(0,t.jsx)("meta",{name:"theme-color",media:"(prefers-color-scheme: light)",content:"#fafaf9"}),(0,t.jsx)("meta",{name:"theme-color",media:"(prefers-color-scheme: dark)",content:"#1a1b1b"}),(0,t.jsx)("meta",{httpEquiv:"origin-trial",content:"AsKJNnBESA8LBSWSiA1TeHAuM7wj/zSm2MVlsxnG6yMeAuorNg9zyAEC3w+lp88yOz+9zkJmIQ++T1Cl4asHoAUAAABQeyJvcmlnaW4iOiJodHRwczovL3JlcGxpdC5jb206NDQzIiwiZmVhdHVyZSI6IkRpZ2l0YWxHb29kcyIsImV4cGlyeSI6MTYzMTY2Mzk5OX0="}),(0,t.jsx)("link",{rel:"apple-touch-icon",href:b}),(0,t.jsx)("meta",{name:"mobile-web-app-capable",content:"yes"}),(0,t.jsx)("meta",{name:"apple-mobile-web-app-title",content:"Replit"}),C?(0,t.jsx)("link",{rel:"canonical",href:C},"rel:canonical"):null]}),(0,t.jsx)(s.HackFontLoader,{}),(0,t.jsx)(l.default,{})]})}])},258090,e=>{e.v({wrapper:"DesktopAppVersion-module__fsz5-q__wrapper"})},621577,e=>{"use strict";var t=e.i(276385),i=e.i(554493),a=e.i(632350);e.i(214847);var o=e.i(864300),n=e.i(8047),r=e.i(61732),s=e.i(258090);e.s(["DesktopAppVersion",0,function(){let e=(0,o.useIntl)();return(0,a.default)()?(0,t.jsx)(r.View,{clsx:s.default.wrapper,py:4,px:8,align:"center",justify:"center",children:(0,t.jsx)(n.Text,{color:"dimmer",variant:"small",dataCy:"desktop-app-version",children:e.formatMessage({id:"components.navMenuDesktopAppVersion",defaultMessage:"Version {version}"},{version:i.desktopAppApi.getVersion()})})}):null}])},816498,e=>{"use strict";var t=e.i(351623);e.i(344480);var i=e.i(975473);let a={},o=t.gql`
    fragment DevCopyUserItemsCurrentUser on CurrentUser {
  id
  username
  email
}
    `,n=t.gql`
    query DevCopyUserItemsCustomer {
  currentUser {
    id
    existingCustomerId
  }
}
    `;e.s(["DevCopyUserItemsCurrentUserFragmentDoc",0,o,"useDevCopyUserItemsCustomerLazyQuery",0,function(e){let t={...a,...e};return i.useLazyQuery(n,t)}])},702523,e=>{"use strict";var t=e.i(276385),i=e.i(816498),a=e.i(96250),o=e.i(166970),n=e.i(945506),r=e.i(320216);e.i(214847);var s=e.i(864300),l=e.i(20639),c=e.i(829950),u=e.i(295231);e.s(["DevCopyUserItems",0,function({currentUser:e}){let d=(0,s.useIntl)(),{showConfirm:m,showWarning:f}=(0,r.default)(),[p]=(0,i.useDevCopyUserItemsCustomerLazyQuery)({fetchPolicy:"network-only"}),g=/^[^@\s]+@(?:replit\.com|repl\.it)$/i.test(e.email),h=(0,n.isZerglingDevServer)()?(0,n.getZerglingConversationId)():null;return"production"!==(0,c.getEnvironmentTier)()||g?(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(u.Separator,{}),(0,t.jsx)(u.MenuHeader,{text:d.formatMessage({id:"components.navMenuDevOnlyHeader",defaultMessage:"Replit-only"})}),(0,t.jsx)(u.MenuItem,{label:d.formatMessage({id:"components.navMenuCopyUsername",defaultMessage:"Copy Username"}),onAction:async()=>{await (0,l.default)(e.username),m(d.formatMessage({id:"components.navMenuUsernameCopiedConfirm",defaultMessage:"Username copied to clipboard"}))},icon:(0,t.jsx)(o.default,{})}),(0,t.jsx)(u.MenuItem,{label:d.formatMessage({id:"components.navMenuCopyUserId",defaultMessage:"Copy User ID"}),onAction:async()=>{await (0,l.default)(String(e.id)),m(d.formatMessage({id:"components.navMenuUserIdCopiedConfirm",defaultMessage:"User ID copied to clipboard"}))},icon:(0,t.jsx)(o.default,{})}),(0,t.jsx)(u.MenuItem,{label:d.formatMessage({id:"components.navMenuCopyCustomerId",defaultMessage:"Copy Customer ID"}),onAction:async()=>{let{data:e}=await p(),t=e?.currentUser?.existingCustomerId;null==t?f(d.formatMessage({id:"components.navMenuCustomerIdUnavailableWarning",defaultMessage:"This user does not have a customer ID"})):(await (0,l.default)(String(t)),m(d.formatMessage({id:"components.navMenuCustomerIdCopiedConfirm",defaultMessage:"Customer ID copied to clipboard"})))},icon:(0,t.jsx)(o.default,{})}),h?(0,t.jsx)(u.MenuItem,{label:d.formatMessage({id:"components.navMenuOpenZergChat",defaultMessage:"Open Zerg chat"}),description:h,href:(0,n.chatUrlFor)(h),target:"_blank",rel:"noopener noreferrer",icon:(0,t.jsx)(a.default,{})}):null]}):null}])},566032,e=>{"use strict";var t=e.i(276385),i=e.i(625251),a=e.i(787527),o=e.i(917255),n=e.i(927600),r=e.i(399245),s=e.i(222878),l=e.i(761201),c=e.i(709485);e.i(214847);var u=e.i(20397),d=e.i(864300),m=e.i(415541),f=e.i(579219),p=e.i(295231),g=e.i(773222),h=e.i(8047),v=e.i(61732);e.s(["HelpItem",0,function({setActiveModal:e,feedbackItem:x,children:y}){let _=(0,d.useIntl)(),w=_.formatMessage({id:"components.navMenuHelp",defaultMessage:"Help"});return(0,t.jsx)(t.Fragment,{children:(0,t.jsxs)(i.SubmenuTrigger,{children:[(0,t.jsx)(p.BaseMenuItem,{textValue:w,children:(0,t.jsxs)(v.View,{align:"center",row:!0,gap:6,justify:"space-between",grow:!0,shrink:!0,children:[(0,t.jsxs)(v.View,{align:"center",grow:!0,shrink:!0,row:!0,gap:6,children:[(0,t.jsx)(s.default,{}),(0,t.jsx)(h.Text,{children:(0,t.jsx)(u.FormattedMessage,{id:"components.navMenuHelp",defaultMessage:"Help"})})]}),(0,t.jsx)(n.default,{size:12})]})}),(0,t.jsx)(g.RawPopover,{offset:4,children:(0,t.jsx)(v.View,{p:4,children:(0,t.jsxs)(p.Menu,{"aria-label":w,children:[(0,t.jsx)(f.StatusItem,{}),(0,t.jsx)(p.MenuItem,{label:_.formatMessage({id:"components.navMenuGetHelp",defaultMessage:"Get help"}),icon:(0,t.jsx)(s.default,{size:16}),onAction:()=>{e("support"),(0,m.track)(c.events.HELP_FORM_OPENED,{type:"New Help Form"})}}),x,(0,t.jsx)(p.MenuItem,{icon:(0,t.jsx)(r.default,{}),label:_.formatMessage({id:"components.navMenuCommunityHub",defaultMessage:"Community Hub"}),href:l.COMMUNITY_URL,target:"_blank",rel:"noreferrer noopener"}),(0,t.jsx)(p.MenuItem,{icon:(0,t.jsx)(o.default,{}),label:_.formatMessage({id:"components.navMenuReadTheDocs",defaultMessage:"Read the docs"}),href:l.LINKS_DOCS.HOME,target:"_blank",rel:"noreferrer noopener",onAction:()=>{(0,m.track)(c.events.DOCS_OPENED,{source:"help_menu"})}}),(0,t.jsx)(p.Separator,{}),(0,t.jsx)(p.MenuItem,{icon:(0,t.jsx)(a.default,{}),label:_.formatMessage({id:"components.navMenuViewChangelog",defaultMessage:"View changelog"}),href:l.LINKS_DOCS.CHANGELOG,target:"_blank",rel:"noreferrer noopener",onAction:()=>{(0,m.track)(c.events.CHANGELOG_OPENED)}}),y]})})})]})})}])},70219,e=>{"use strict";var t=e.i(276385),i=e.i(408116),a=e.i(98346),o=e.i(709485),n=e.i(443197),r=e.i(554493),s=e.i(632350);e.i(214847);var l=e.i(864300),c=e.i(415541),u=e.i(295231);e.s(["LogoutItem",0,function(){let e=(0,l.useIntl)(),d=(0,i.useApolloClient)(),m=(0,s.default)();return(0,t.jsx)(u.MenuItem,{label:e.formatMessage({id:"components.navMenuLogOut",defaultMessage:"Log out"}),icon:(0,t.jsx)(a.default,{}),onAction:async()=>{if(m)return void r.desktopAppApi.logout();let e=window.analytics?.user?.()?.anonymousId?.()??"";(0,c.trackV2)(o.eventsV2.LOGOUT_COMPLETED,{pre_logout_anonymous_id:e}),await (0,n.signOut)(d),window.location.href="/logout"}})}])},638046,e=>{e.v({count:"NotificationsItem-module__00pi2G__count"})},165887,e=>{"use strict";var t=e.i(276385),i=e.i(648880);e.i(214847);var a=e.i(20397),o=e.i(864300),n=e.i(919073),r=e.i(295231),s=e.i(8047),l=e.i(61732),c=e.i(638046);e.s(["NotificationsItem",0,function({count:e,onAction:u}){let d=(0,o.useIntl)().formatMessage({id:"components.navMenuNotifications",defaultMessage:"Notifications"});return(0,t.jsx)(r.BaseMenuItem,{textValue:d,onAction:u,children:(0,t.jsxs)(l.View,{align:"center",row:!0,gap:6,justify:"space-between",grow:!0,shrink:!0,children:[(0,t.jsxs)(l.View,{align:"center",grow:!0,shrink:!0,row:!0,gap:6,children:[(0,t.jsx)(i.default,{}),(0,t.jsx)(s.Text,{children:(0,t.jsx)(a.FormattedMessage,{id:"components.navMenuNotifications",defaultMessage:"Notifications"})})]}),e?(0,t.jsx)(n.ShadesSurface,{clsx:c.default.count,colorShade:"themeError",align:"center",justify:"center",children:(0,t.jsx)(s.Text,{variant:"small",children:e})}):null]})})}])},767913,e=>{"use strict";var t=e.i(276385),i=e.i(625251),a=e.i(927600),o=e.i(813707),n=e.i(411966);e.i(214847);var r=e.i(20397),s=e.i(864300),l=e.i(86145),c=e.i(295231),u=e.i(773222),d=e.i(8047),m=e.i(61732);e.s(["PerfMenuItem",0,function(){let e=(0,s.useIntl)(),[f,p]=(0,n.usePerfToggle)(n.PERF_TOGGLE_KEYS.lagRadar),[g,h]=(0,n.usePerfToggle)(n.PERF_TOGGLE_KEYS.analyticsInspector),[v,x]=(0,n.usePerfToggle)(n.PERF_TOGGLE_KEYS.devBar),[y,_]=(0,n.usePerfToggle)(n.PERF_TOGGLE_KEYS.ruiAudit),w=e.formatMessage({id:"components.perfButtonPerformance",defaultMessage:"Performance"}),b=e.formatMessage({id:"components.perfButtonLagRadar",defaultMessage:"Lag Radar"}),C=e.formatMessage({id:"components.perfButtonAnalyticsInspector",defaultMessage:"Analytics Inspector"}),N=e.formatMessage({id:"components.perfButtonDevBar",defaultMessage:"Dev bar"});return e.formatMessage({id:"components.perfButtonRuiAudit",defaultMessage:"RUI audit"}),(0,t.jsxs)(i.SubmenuTrigger,{children:[(0,t.jsx)(c.BaseMenuItem,{textValue:w,children:(0,t.jsxs)(m.View,{align:"center",row:!0,gap:6,justify:"space-between",grow:!0,shrink:!0,children:[(0,t.jsxs)(m.View,{align:"center",grow:!0,shrink:!0,row:!0,gap:6,children:[(0,t.jsx)(o.default,{}),(0,t.jsx)(d.Text,{multiline:!1,children:(0,t.jsx)(r.FormattedMessage,{id:"components.perfButtonPerformance",defaultMessage:"Performance"})})]}),(0,t.jsx)(a.default,{size:12})]})}),(0,t.jsx)(u.RawPopover,{offset:4,children:(0,t.jsxs)(m.View,{gap:4,p:8,children:[(0,t.jsxs)(m.View,{row:!0,gap:8,children:[(0,t.jsx)(l.Checkbox,{id:"lag-radar",name:b,checked:f,onChange:p}),(0,t.jsx)("label",{htmlFor:"lag-radar",children:b})]}),(0,t.jsxs)(m.View,{row:!0,gap:8,children:[(0,t.jsx)(l.Checkbox,{id:"analytics-inspector",name:C,checked:g,onChange:h}),(0,t.jsx)("label",{htmlFor:"analytics-inspector",children:C})]}),(0,t.jsxs)(m.View,{row:!0,gap:8,children:[(0,t.jsx)(l.Checkbox,{id:"dev-bar",name:N,checked:v,onChange:x}),(0,t.jsx)("label",{htmlFor:"dev-bar",children:N})]}),null]})})]})}])},579219,e=>{"use strict";var t=e.i(276385),i=e.i(519979);e.i(214847);var a=e.i(864300),o=e.i(295231);e.s(["StatusItem",0,function(){let e=(0,a.useIntl)();return(0,t.jsx)(o.MenuItem,{icon:(0,t.jsx)(i.default,{}),label:e.formatMessage({id:"components.navMenuStatus",defaultMessage:"Status"}),href:"https://status.replit.com"})}])},13465,e=>{"use strict";var t=e.i(276385),i=e.i(625251),a=e.i(927600),o=e.i(155119),n=e.i(759317),r=e.i(393428),s=e.i(308521),l=e.i(632350);e.i(214847);var c=e.i(20397),u=e.i(864300),d=e.i(401036),m=e.i(841114),f=e.i(295231),p=e.i(773222),g=e.i(8047),h=e.i(61732);e.s(["ThemeItem",0,function(){let e=(0,u.useIntl)(),v=(0,l.default)(),{currentTheme:x}=(0,d.useTheme)(),{setActiveTheme:y,isSystemTheme:_}=(0,m.useThemePreference)();if(v)return null;let w=e.formatMessage({id:"components.navMenuTheme",defaultMessage:"Theme"}),b=e.formatMessage({id:"components.navMenuThemeLight",defaultMessage:"Light"}),C=e.formatMessage({id:"components.navMenuThemeDark",defaultMessage:"Dark"}),N=e.formatMessage({id:"components.navMenuThemeSystem",defaultMessage:"System"}),M=e.formatMessage({id:"components.navMenuThemeCustom",defaultMessage:"Custom"});return _?M=N:"replitDark"===x.id?M=C:"replitLight"===x.id&&(M=b),(0,t.jsxs)(i.SubmenuTrigger,{children:[(0,t.jsx)(f.BaseMenuItem,{textValue:w,children:(0,t.jsxs)(h.View,{align:"center",row:!0,gap:6,justify:"space-between",grow:!0,shrink:!0,children:[(0,t.jsxs)(h.View,{align:"center",grow:!0,shrink:!0,row:!0,gap:6,children:[(0,t.jsx)(r.default,{}),(0,t.jsx)(g.Text,{multiline:!1,children:(0,t.jsx)(c.FormattedMessage,{id:"components.navMenuTheme",defaultMessage:"Theme"})})]}),(0,t.jsxs)(h.View,{row:!0,gap:4,align:"center",children:[(0,t.jsx)(g.Text,{color:"dimmer",children:M}),(0,t.jsx)(a.default,{size:12})]})]})}),(0,t.jsx)(p.RawPopover,{offset:4,children:(0,t.jsx)(h.View,{p:4,children:(0,t.jsxs)(f.Menu,{"aria-label":w,children:[(0,t.jsx)(f.MenuItem,{label:b,icon:(0,t.jsx)(s.default,{size:16}),onAction:()=>y("replitLight")}),(0,t.jsx)(f.MenuItem,{label:C,icon:(0,t.jsx)(n.default,{size:16}),onAction:()=>y("replitDark")}),(0,t.jsx)(f.MenuItem,{label:N,icon:(0,t.jsx)(o.default,{size:16}),onAction:()=>y("system")})]})})})]})}])},720293,e=>{e.v({hero:"ProPlanHero-module__U5Xbua__hero"})},92142,e=>{"use strict";var t=e.i(276385);e.i(214847);var i=e.i(864300),a=e.i(401036),o=e.i(727223),n=e.i(720293);e.s(["ProPlanHero",0,function(){let e=(0,i.useIntl)(),{isDarkColorScheme:r}=(0,a.useTheme)();return(0,t.jsx)(o.default,{clsx:n.default.hero,src:r?"/public/images/upgrade/pro-plan-hero-dark.svg":"/public/images/upgrade/pro-plan-hero-light.svg",width:480,height:240,loading:"eager",alt:e.formatMessage({id:"billing.proPlanHeroAlt",defaultMessage:"Replit Pro plan benefits: more credits and discounts to build, priority support, more powerful models, and work with up to 10 parallel agents."})})}])},78768,e=>{e.v({learnMoreLink:"SubscriptionPauseBanner-module__psmYua__learnMoreLink"})},805581,e=>{"use strict";var t=e.i(276385),i=e.i(761201),a=e.i(884214);e.i(214847);var o=e.i(614852),n=e.i(20397),r=e.i(864300),s=e.i(242917),l=e.i(557113),c=e.i(643484),u=e.i(108431),d=e.i(244945),m=e.i(124298),f=e.i(78768);function p(e){let t=(0,l.getScheduledPauseSubscription)(e??null);if(t)return{status:"scheduled",subscription:t};let i=(0,l.getPausedSubscription)(e??null);return i?.resumesAt!=null?{status:"paused",subscription:{...i,resumesAt:i.resumesAt}}:null}e.s(["SubscriptionPauseBanner",0,function({arrangement:e,customerId:g,variant:h,disabledReason:v,onDismiss:x}){let y=(0,r.useIntl)(),{show:_}=(0,s.useGlobalModal)(),w=(0,a.useAutoLogView)({elementId:"home_subscription_pause_banner"}),b=(0,a.useAutoLogView)({elementId:"billing_subscription_pause_banner"}),C=p(e);if(!C)return null;let N=(0,l.getPlanDisplayName)(C.subscription,y),M="scheduled"===C.status?y.formatMessage({id:"settings.billingV2.subscriptionPauseScheduled",defaultMessage:"Your {planName} subscription is scheduled to pause. You can cancel the scheduled pause until {date}."},{planName:N,date:(0,o.formatDate)(C.subscription.subscriptionEndDate,{locale:y.locale})}):y.formatMessage({id:"settings.billingV2.subscriptionPausedUntil",defaultMessage:"Your {planName} subscription is paused until {date}."},{planName:N,date:(0,o.formatDate)(C.subscription.resumesAt,{locale:y.locale})}),I="scheduled"===C.status?(0,t.jsx)(d.Tooltip,{tooltip:v,isDisabled:!v,children:(0,t.jsx)(c.Button,{size:"small",variant:"outlined",disabled:!!v,"data-analytics-id":"top"===h?"home_page_scheduled_pause_banner_cancel_button":"billing_page_scheduled_pause_banner_cancel_button",text:y.formatMessage({id:"settings.billingV2.cancelScheduledPause",defaultMessage:"Cancel pause"}),onClick:()=>{_("CancelScheduledPauseModal",{customerId:g})}})}):null;if("top"===h)return(0,t.jsx)(m.TopBanner,{dataAnalyticsId:"home_subscription_pause_banner",dismissButtonAnalyticsId:"home_subscription_pause_banner_dismiss_button",colorway:"themeNotice",text:M,onDismiss:x,innerRef:w,children:I});let j=e=>(0,t.jsx)("a",{"data-analytics-id":"billing_subscription_pause_banner_learn_more_link",href:i.LINKS_DOCS.SUBSCRIPTION_PAUSE,target:"_blank",rel:"noopener noreferrer",clsx:f.default.learnMoreLink,children:e}),R="paused"===C.status?(0,t.jsx)(n.FormattedMessage,{id:"settings.billingV2.subscriptionPausedUntilLearnMore",defaultMessage:"Your {planName} subscription is paused until {date}. <link>Learn more</link>",values:{planName:N,date:(0,o.formatDate)(C.subscription.resumesAt,{locale:y.locale}),link:j}}):(0,t.jsx)(n.FormattedMessage,{id:"settings.billingV2.subscriptionPauseScheduledLearnMore",defaultMessage:"Your {planName} subscription is scheduled to pause. You can cancel the scheduled pause until {date}. <link>Learn more</link>",values:{planName:N,date:(0,o.formatDate)(C.subscription.subscriptionEndDate,{locale:y.locale}),link:j}});return(0,t.jsx)(u.StatusBanner,{dataAnalyticsId:"billing_subscription_pause_banner",colorway:"blue",text:R,action:I??void 0,innerRef:b})},"getSubscriptionPauseBannerPause",0,p])},557113,e=>{"use strict";e.s(["getPausedSubscription",0,function(e){return e?.__typename!=="CustomerSelfServeSubscription"||"PAUSED"!==e.pauseStatus||e.cancellationScheduledAt?null:e},"getPlanDisplayName",0,function(e,t){if("CustomerSelfServeSubscription"===e.__typename){let{plan:t}=e;return"TieredSelfServePlan"===t.__typename?`${t.name} ${t.tier}`:t.name}return"CustomerSalesContract"===e.__typename?e.name:t.formatMessage({id:"settings.billingV2.freePlan",defaultMessage:"Free"})},"getScheduledPauseSubscription",0,function(e){return e?.__typename==="CustomerSelfServeSubscription"&&"SCHEDULED"===e.pauseStatus?e:null}])},411966,e=>{"use strict";var t=e.i(389959),i=e.i(489859);e.s(["PERF_TOGGLE_KEYS",0,{lagRadar:"perf-tools-lag-radar",fpsCounter:"perf-tools-fps-counter",analyticsInspector:"analyticsInspector",devBar:"devbar-visible",ruiAudit:"rui-audit-visible"},"usePerfToggle",0,function(e,a=!1){let[o,n]=(0,t.useState)(()=>{let t;return null===(t=i.default.get(e,"boolean"))?a:t});return(0,t.useEffect)(()=>{let t=t=>{t.key===e&&n("true"===t.newValue)};return window.addEventListener("storage",t),()=>window.removeEventListener("storage",t)},[e]),[o,t=>{n(t),i.default.set(e,t),window.dispatchEvent(new StorageEvent("storage",{key:e,newValue:String(t)}))}]}])},448656,e=>{"use strict";var t=e.i(351623),i=e.i(344480);e.i(975473);let a={},o=t.gql`
    query DevBarGate {
  currentUser {
    __typename
    id
    isStaff: hasRole(role: REPLIT_STAFF)
    isExplorer: hasRole(role: EXPLORER)
  }
}
    `;e.s(["useDevBarGateQuery",0,function(e){let t={...a,...e};return i.useQuery(o,t)}])},952233,e=>{"use strict";var t=e.i(15801),i=e.i(448656);e.s(["useShouldShowDevBar",0,function(e={}){let a="1"===(0,t.useRouter)().query.debug,{knownUnauthed:o=!1}=e,{data:n}=(0,i.useDevBarGateQuery)({skip:a||o});return!!a||!o&&n?.currentUser?.__typename==="CurrentUser"&&n.currentUser.isStaff&&n.currentUser.isExplorer}])},945506,e=>{"use strict";var t=e.i(365669);e.s(["chatUrlFor",0,function(e){return`https://zerg.zergrush.dev/chat?id=${encodeURIComponent(e)}`},"getZerglingConversationId",0,function(){return t.publicEnv.ZERG_CONVERSATION_ID??null},"isZerglingDevServer",0,function(){return!1}])},233763,e=>{"use strict";var t=e.i(389959),i=e.i(830675),a=e.i(320216),o=e.i(871752),n=e.i(489859);let r="email-verification-resend-timestamp";e.s(["useEmailVerificationResend",0,function(){let[e,s]=(0,t.useState)(!1),[l,c]=(0,t.useState)(0),{showConfirm:u,showError:d}=(0,a.default)();return(0,t.useEffect)(()=>{try{let e=n.default.get(r,"number");if(e){let t=Date.now()-e;t<6e4?(s(!0),c(Math.ceil((6e4-t)/1e3))):n.default.remove(r)}}catch(e){i.captureException(e)}},[]),(0,t.useEffect)(()=>{if(!e||l<=0)return;let t=setInterval(()=>{c(e=>{let a=e-1;if(a<=0){s(!1);try{n.default.remove(r)}catch(e){i.captureException(e)}return clearInterval(t),0}return a})},1e3);return()=>clearInterval(t)},[e,l]),{resendVerification:(0,t.useCallback)(async()=>{if(!e)try{await (0,o.postJson)("/data/user/resend_verification",{}),u("Verification email sent"),s(!0),c(60);try{n.default.set(r,Date.now())}catch(e){i.captureException(e)}}catch(t){let{message:e}=t;d(`Failed to resend verification email: ${e}`),i.captureException(t)}},[e,u,d]),isInCooldown:e,cooldownTimeRemaining:l}}])},160344,e=>{"use strict";var t=e.i(389959),i=e.i(489859);e.s(["default",0,function(e,a){let[o,n]=(0,t.useState)(a),[r,s]=(0,t.useState)(!1);return(0,t.useEffect)(()=>{let t=i.default.get(e);null!=t&&n(t),s(!0)},[e]),(0,t.useEffect)(()=>{r&&i.default.set(e,o)},[e,o,r]),[o,n,r]}])},890630,e=>{"use strict";let t="/public/icons/favicon-prompt-";e.s(["rebrandFavicons",0,{prompt:t+"192-rebrand.png",agentWaiting:t+"agent-waiting-192-rebrand.png",agentWorking:t+"agent-working-192-rebrand.png",agentFinished:t+"agent-finished-192-rebrand.png"}])},886527,e=>{"use strict";var t=e.i(229952);e.s(["default",0,({host:e,protocol:i},a)=>{let{hash:o,query:n,pathname:r}=a?(0,t.parse)(a,!0):{hash:null,query:{},pathname:"/"};return(0,t.format)({protocol:i,host:e,hash:o,pathname:r,query:n})}])},562821,e=>{"use strict";e.s(["default",0,()=>({href:"/home",as:"/~"})])},871752,e=>{"use strict";var t=e.i(324753),i=e.i(272391);function a(e,i){return(0,t.default)(e,{credentials:"same-origin",headers:{"Content-Type":"application/json",Accept:"application/json","X-Requested-With":"XMLHttpRequest"},method:"post",body:JSON.stringify(i)})}e.s(["postJson",0,function(e,t={}){var o,n;let r;return o=a(e,t),n=e,r=new i.default("Unknown http error"),Promise.resolve(o).then(async e=>{let t;if(e.ok)return e.json();let i=e.headers.get("content-type");if(i&&i.includes("application/json"))t=await e.json();else{let i=await e.text();try{t=JSON.parse(i)}catch(e){t={message:i}}}throw t.message&&(r.message=t.message),r.setExtras({url:n,responseBody:t,responseData:{status:e.status,statusText:e.statusText,redirected:e.redirected,type:e.type,url:e.url}}).setTag("httpError","true"),r})},"wrapPost",0,a])},21419,e=>{"use strict";var t=e.i(389959);async function i(){if(!("u"<typeof document)&&document.hidden)return new Promise(e=>{let t=()=>{document.hidden||(document.removeEventListener("visibilitychange",t),e())};document.addEventListener("visibilitychange",t)})}e.s(["pageVisible",0,i,"usePageVisibility",0,function(){let[e,i]=(0,t.useState)(!1);return(0,t.useEffect)(()=>{function e(){i(!document.hidden)}return document.addEventListener("visibilitychange",e),e(),()=>{document.removeEventListener("visibilitychange",e)}},[]),e},"useRefetchOnPageVisible",0,function(e){let i=(0,t.useRef)(e);i.current=e,(0,t.useEffect)(()=>{function e(){document.hidden||i.current()}return document.addEventListener("visibilitychange",e),()=>{document.removeEventListener("visibilitychange",e)}},[])}])},405779,e=>{"use strict";var t=e.i(351623);let i=t.gql`
    fragment NotificationItemCreator on User {
  id
  image
  username
  fullName
  url
}
    `,a=t.gql`
    fragment NotificationItemRepliedToPostNotification on RepliedToPostNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,o=t.gql`
    fragment NotificationItemRepliedToCommentNotification on RepliedToCommentNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,n=t.gql`
    fragment NotificationItemMentionedInPostNotification on MentionedInPostNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,r=t.gql`
    fragment NotificationItemMentionedInCommentNotification on MentionedInCommentNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,s=t.gql`
    fragment NotificationItemAnswerAcceptedNotification on AnswerAcceptedNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,l=t.gql`
    fragment NotificationItemMultiplayerJoinedEmailNotification on MultiplayerJoinedEmailNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,c=t.gql`
    fragment NotificationItemMultiplayerJoinedLinkNotification on MultiplayerJoinedLinkNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,u=t.gql`
    fragment NotificationItemMultiplayerInvitedNotification on MultiplayerInvitedNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,d=t.gql`
    fragment NotificationItemMultiplayerOverlimitNotification on MultiplayerOverlimitNotification {
  id
  text
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,m=t.gql`
    fragment NotificationItemWarningNotification on WarningNotification {
  id
  text
  url
  timeCreated
  seen
}
    `,f=t.gql`
    fragment NotificationItemTeamInvite on TeamInvite {
  id
  team {
    id
    displayName
    username
  }
}
    `,p=t.gql`
    fragment NotificationItemTeamInviteNotification on TeamInviteNotification {
  id
  text
  url
  timeCreated
  seen
  invite {
    id
    ...NotificationItemTeamInvite
  }
}
    ${f}`,g=t.gql`
    fragment NotificationItemTeamOrganizationInvite on TeamOrganizationInvite {
  id
  organization {
    id
    name
  }
}
    `,h=t.gql`
    fragment NotificationItemTeamOrganizationInviteNotification on TeamOrganizationInviteNotification {
  id
  text
  url
  timeCreated
  seen
  invite {
    id
    ...NotificationItemTeamOrganizationInvite
  }
}
    ${g}`,v=t.gql`
    fragment NotificationTeamTemplateSubmittedNotification on TeamTemplateSubmittedNotification {
  id
  text
  url
  timeCreated
  seen
  repl {
    id
    url
  }
}
    `,x=t.gql`
    fragment NotificationTeamTemplateReviewedStatusNotification on TeamTemplateReviewedStatusNotification {
  id
  text
  url
  timeCreated
  seen
  repl {
    id
    url
  }
}
    `,y=t.gql`
    fragment NotificationReplCommentCreatedNotification on ReplCommentCreatedNotification {
  id
  url
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,_=t.gql`
    fragment NotificationReplCommentReplyCreatedNotification on ReplCommentReplyCreatedNotification {
  id
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,w=t.gql`
    fragment NotificationReplCommentMentionNotification on ReplCommentMentionNotification {
  id
  timeCreated
  seen
  creator {
    id
    ...NotificationItemCreator
  }
}
    ${i}`,b=t.gql`
    fragment NotificationItemNewFollower on NewFollowerNotification {
  id
  timeCreated
  seen
  url
  creator {
    ...NotificationItemCreator
  }
}
    ${i}`,C=t.gql`
    fragment BasicNotificationItemNotification on BasicNotification {
  id
  text
  url
  timeCreated
  seen
  context
}
    `,N=t.gql`
    fragment NotificationItemEgressLimitNotification on EgressLimitNotification {
  id
  url
  timeCreated
  seen
  variant
  limitGib
  percentage
}
    `,M=t.gql`
    fragment NotificationItemOrgUpgradeRequestReviewedNotification on OrgUpgradeRequestReviewedNotification {
  id
  timeCreated
  seen
  url
  creator {
    ...NotificationItemCreator
  }
  isAccepted
  orgId
}
    ${i}`,I=t.gql`
    fragment NotificationItemIntegrationRequestedNotification on IntegrationRequestedNotification {
  id
  timeCreated
  seen
  url
  creator {
    ...NotificationItemCreator
  }
  connectorName
  message
}
    ${i}`,j=t.gql`
    fragment NotificationItemIntegrationRequestReviewedNotification on IntegrationRequestReviewedNotification {
  id
  timeCreated
  seen
  url
  creator {
    ...NotificationItemCreator
  }
  connectorName
  isApproved
}
    ${i}`;e.s(["BasicNotificationItemNotificationFragmentDoc",0,C,"NotificationItemAnswerAcceptedNotificationFragmentDoc",0,s,"NotificationItemEgressLimitNotificationFragmentDoc",0,N,"NotificationItemIntegrationRequestReviewedNotificationFragmentDoc",0,j,"NotificationItemIntegrationRequestedNotificationFragmentDoc",0,I,"NotificationItemMentionedInCommentNotificationFragmentDoc",0,r,"NotificationItemMentionedInPostNotificationFragmentDoc",0,n,"NotificationItemMultiplayerInvitedNotificationFragmentDoc",0,u,"NotificationItemMultiplayerJoinedEmailNotificationFragmentDoc",0,l,"NotificationItemMultiplayerJoinedLinkNotificationFragmentDoc",0,c,"NotificationItemMultiplayerOverlimitNotificationFragmentDoc",0,d,"NotificationItemNewFollowerFragmentDoc",0,b,"NotificationItemOrgUpgradeRequestReviewedNotificationFragmentDoc",0,M,"NotificationItemRepliedToCommentNotificationFragmentDoc",0,o,"NotificationItemRepliedToPostNotificationFragmentDoc",0,a,"NotificationItemTeamInviteNotificationFragmentDoc",0,p,"NotificationItemTeamOrganizationInviteNotificationFragmentDoc",0,h,"NotificationItemWarningNotificationFragmentDoc",0,m,"NotificationReplCommentCreatedNotificationFragmentDoc",0,y,"NotificationReplCommentMentionNotificationFragmentDoc",0,w,"NotificationReplCommentReplyCreatedNotificationFragmentDoc",0,_,"NotificationTeamTemplateReviewedStatusNotificationFragmentDoc",0,x,"NotificationTeamTemplateSubmittedNotificationFragmentDoc",0,v])},987987,e=>{e.v({content:"Item-module__2hGgkG__content",contentContainer:"Item-module__2hGgkG__contentContainer",itemText:"Item-module__2hGgkG__itemText",itemTextNewFollowerWrapper:"Item-module__2hGgkG__itemTextNewFollowerWrapper",userAvatarLink:"Item-module__2hGgkG__userAvatarLink",usernameLink:"Item-module__2hGgkG__usernameLink"})},378099,e=>{"use strict";var t=e.i(276385),i=e.i(36454);e.i(214847);var a=e.i(20397),o=e.i(864300),n=e.i(377498),r=e.i(192915),s=e.i(825419),l=e.i(8047),c=e.i(472499),u=e.i(61732),d=e.i(987987);let m=({seen:e,url:o,as:m,href:f,text:p,timeCreated:g,creator:h,invite:v,orgInvite:x,isLastItem:y=!1,..._})=>{let w,b;return o&&((w=new URL("/"===o[0]?`https://replit.com${o}`:o)).searchParams.set("from","notifications"),b=(o.startsWith("http")&&"https:"===w.protocol?w.protocol+"//"+w.hostname:"")+w.pathname+w.search+w.hash),(0,t.jsx)(n.default,{seen:e,compact:_.compact,isLastItem:y,as:m,href:f||b,children:(0,t.jsxs)("div",{clsx:d.default.contentContainer,children:[h?(0,t.jsx)(i.default,{...(0,r.userLinkProps)(h),clsx:d.default.userAvatarLink,children:(0,t.jsx)(s.Avatar,{size:32,src:h.image,username:h.username,fullName:h.fullName})}):null,(0,t.jsxs)(u.View,{shrink:!0,children:[(0,t.jsxs)("div",{clsx:d.default.itemText,children:[h?(0,t.jsx)(i.default,{...(0,r.userLinkProps)(h),clsx:d.default.usernameLink,children:h.username}):null," ",p,v?(0,t.jsx)(a.FormattedMessage,{id:"notifications.inviteSuffix",defaultMessage:" {teamName}. Click here to join",values:{teamName:v.team.displayName}}):null,x?(0,t.jsx)(a.FormattedMessage,{id:"notifications.orgInviteSuffix",defaultMessage:" the {orgName} workspace. Click here to join",values:{orgName:x.organization.name}}):null]}),(0,t.jsx)(l.Text,{variant:"small",color:"dimmer",multiline:!1,children:(0,t.jsx)(c.Timestamp,{date:g})})]})]})})};e.s(["default",0,({notification:e,isLastItem:u,setAsSeen:f,...p})=>{let g=(0,o.useIntl)();if("ReplCommentCreatedNotification"===e.__typename||"ReplCommentReplyCreatedNotification"===e.__typename||"ReplCommentMentionNotification"===e.__typename){let i={ReplCommentCreatedNotification:g.formatMessage({id:"notifications.commentedOnRepl",defaultMessage:"commented on your repl"}),ReplCommentReplyCreatedNotification:g.formatMessage({id:"notifications.repliedToCommentOnRepl",defaultMessage:"replied to your comment on your repl"}),ReplCommentMentionNotification:g.formatMessage({id:"notifications.mentionedInRepl",defaultMessage:"mentioned you in your repl"})}[e.__typename];return(0,t.jsx)(m,{isLastItem:u,text:i||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,creator:e.creator||void 0})}if("BasicNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:e.text||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("MentionedInPostNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:g.formatMessage({id:"notifications.mentionedInPost",defaultMessage:"mentioned you in their post"}),creator:e.creator||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("MentionedInCommentNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:g.formatMessage({id:"notifications.mentionedInComment",defaultMessage:"mentioned you in their comment"}),creator:e.creator||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("RepliedToPostNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:g.formatMessage({id:"notifications.repliedToPost",defaultMessage:"replied to your post"}),creator:e.creator||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("RepliedToCommentNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:g.formatMessage({id:"notifications.repliedToComment",defaultMessage:"replied to your comment"}),creator:e.creator||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("AnswerAcceptedNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:g.formatMessage({id:"notifications.answerAccepted",defaultMessage:"accepted your answer (you earned 5 cycles!)"}),creator:e.creator||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("WarningNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:g.formatMessage({id:"notifications.warning",defaultMessage:"You have been warned by a moderator.  Click here to learn more."}),seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("TeamInviteNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:g.formatMessage({id:"notifications.invitedToJoin",defaultMessage:"You have been invited to join"}),invite:e.invite||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("TeamOrganizationInviteNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:g.formatMessage({id:"notifications.invitedToJoin",defaultMessage:"You have been invited to join"}),orgInvite:e.invite||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("MultiplayerJoinedEmailNotification"===e.__typename||"MultiplayerJoinedLinkNotification"===e.__typename||"MultiplayerInvitedNotification"===e.__typename||"MultiplayerOverlimitNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:e.text?e.text.split(" ").slice(1).join(" "):"",creator:e.creator||void 0,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url});if("TeamTemplateSubmittedNotification"===e.__typename||"TeamTemplateReviewedStatusNotification"===e.__typename)return(0,t.jsx)(m,{isLastItem:u,text:e.text,seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.repl?.url||e.url});if("NewFollowerNotification"===e.__typename){let o=e.creator?(0,r.userLinkProps)(e.creator):null;return(0,t.jsx)(n.default,{seen:f||e.seen,compact:p.compact,as:o?.as,href:o?.href,isLastItem:u,children:(0,t.jsxs)("div",{clsx:d.default.contentContainer,children:[(0,t.jsx)(s.Avatar,{size:32,src:e.creator?.image??null,username:e.creator?.username??"",fullName:e.creator?.fullName}),(0,t.jsx)("div",{clsx:d.default.content,children:(0,t.jsx)("div",{clsx:d.default.itemTextNewFollowerWrapper,children:(0,t.jsxs)("div",{clsx:d.default.itemText,children:[(0,t.jsxs)("div",{children:[e.creator?(0,t.jsx)(i.default,{...(0,r.userLinkProps)(e.creator),children:e.creator.username}):g.formatMessage({id:"notifications.deletedUser",defaultMessage:"[deleted]"})," ",(0,t.jsx)(a.FormattedMessage,{id:"notifications.startedFollowing",defaultMessage:"started following you"})]}),(0,t.jsx)(l.Text,{variant:"small",color:"dimmer",multiline:!1,children:(0,t.jsx)(c.Timestamp,{date:e.timeCreated})})]})})})]})})}return"OrgUpgradeRequestReviewedNotification"===e.__typename?(0,t.jsx)(m,{creator:e.creator||void 0,isLastItem:u,text:e.isAccepted?g.formatMessage({id:"notifications.upgradeRequestApproved",defaultMessage:"approved your request to join the workspace"}):g.formatMessage({id:"notifications.upgradeRequestRejected",defaultMessage:"rejected your request to join the workspace"}),seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url}):"EgressLimitNotification"===e.__typename?(0,t.jsx)(m,{creator:void 0,isLastItem:u,text:"egress_reached_limit"===e.variant?g.formatMessage({id:"notifications.egressReachedLimit",defaultMessage:"You have reached your data transfer limit for the month. Your Apps' data transfer is being throttled, and will be shut off. Upgrade your plan or purchase additional data transfer with Cycles to resume normal speeds."}):g.formatMessage({id:"notifications.egressApproachingLimit",defaultMessage:"You have used {percentage}% of your monthly data transfer limit. If you reach the limit, your Apps data transfer will be throttled and eventually shut off. Upgrade your plan or purchase additional data transfer with Cycles to prevent disruptions to your Apps."},{percentage:e.percentage}),seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url}):"IntegrationRequestedNotification"===e.__typename?(0,t.jsx)(m,{creator:e.creator||void 0,isLastItem:u,text:e.message?g.formatMessage({id:"notifications.integrationRequestedWithMessage",defaultMessage:"has requested the {connectorName} integration: {message}"},{connectorName:e.connectorName,message:e.message}):g.formatMessage({id:"notifications.integrationRequested",defaultMessage:"has requested the {connectorName} integration"},{connectorName:e.connectorName}),seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url}):"IntegrationRequestReviewedNotification"===e.__typename?(0,t.jsx)(m,{creator:e.creator||void 0,isLastItem:u,text:e.isApproved?g.formatMessage({id:"notifications.integrationRequestApproved",defaultMessage:"approved your request for the {connectorName} integration"},{connectorName:e.connectorName}):g.formatMessage({id:"notifications.integrationRequestDenied",defaultMessage:"denied your request for the {connectorName} integration"},{connectorName:e.connectorName}),seen:f||e.seen,compact:p.compact,timeCreated:e.timeCreated,url:e.url}):null}])},295819,e=>{"use strict";var t=e.i(351623),i=e.i(299020);let a={},o=t.gql`
    mutation MarkNotificationsAsSeen($ids: [Int!]) {
  markNotificationsAsSeen(ids: $ids)
}
    `;e.s(["useMarkNotificationsAsSeenMutation",0,function(e){let t={...a,...e};return i.useMutation(o,t)}])},237090,e=>{"use strict";var t=e.i(351623),i=e.i(405779),a=e.i(344480);e.i(975473);let o={},n=t.gql`
    fragment NotificationItems on Notification {
  ... on BasicNotification {
    id
    ...BasicNotificationItemNotification
  }
  ... on MentionedInPostNotification {
    id
    ...NotificationItemMentionedInPostNotification
  }
  ... on RepliedToPostNotification {
    id
    ...NotificationItemRepliedToPostNotification
  }
  ... on MentionedInCommentNotification {
    id
    ...NotificationItemMentionedInCommentNotification
  }
  ... on RepliedToCommentNotification {
    id
    ...NotificationItemRepliedToCommentNotification
  }
  ... on AnswerAcceptedNotification {
    id
    ...NotificationItemAnswerAcceptedNotification
  }
  ... on MultiplayerInvitedNotification {
    id
    ...NotificationItemMultiplayerInvitedNotification
  }
  ... on MultiplayerJoinedEmailNotification {
    id
    ...NotificationItemMultiplayerJoinedEmailNotification
  }
  ... on MultiplayerJoinedLinkNotification {
    id
    ...NotificationItemMultiplayerJoinedLinkNotification
  }
  ... on MultiplayerOverlimitNotification {
    id
    ...NotificationItemMultiplayerOverlimitNotification
  }
  ... on WarningNotification {
    id
    ...NotificationItemWarningNotification
  }
  ... on TeamInviteNotification {
    id
    ...NotificationItemTeamInviteNotification
  }
  ... on TeamOrganizationInviteNotification {
    id
    ...NotificationItemTeamOrganizationInviteNotification
  }
  ... on TeamTemplateSubmittedNotification {
    id
    ...NotificationTeamTemplateSubmittedNotification
  }
  ... on TeamTemplateReviewedStatusNotification {
    id
    ...NotificationTeamTemplateReviewedStatusNotification
  }
  ... on ReplCommentCreatedNotification {
    id
    ...NotificationReplCommentCreatedNotification
  }
  ... on ReplCommentReplyCreatedNotification {
    id
    ...NotificationReplCommentReplyCreatedNotification
  }
  ... on ReplCommentMentionNotification {
    id
    ...NotificationReplCommentMentionNotification
  }
  ... on NewFollowerNotification {
    id
    ...NotificationItemNewFollower
  }
  ... on OrgUpgradeRequestReviewedNotification {
    id
    ...NotificationItemOrgUpgradeRequestReviewedNotification
  }
  ... on EgressLimitNotification {
    id
    ...NotificationItemEgressLimitNotification
  }
  ... on OrgUpgradeRequestReviewedNotification {
    id
    ...NotificationItemOrgUpgradeRequestReviewedNotification
  }
  ... on IntegrationRequestedNotification {
    id
    ...NotificationItemIntegrationRequestedNotification
  }
  ... on IntegrationRequestReviewedNotification {
    id
    ...NotificationItemIntegrationRequestReviewedNotification
  }
}
    ${i.BasicNotificationItemNotificationFragmentDoc}
${i.NotificationItemMentionedInPostNotificationFragmentDoc}
${i.NotificationItemRepliedToPostNotificationFragmentDoc}
${i.NotificationItemMentionedInCommentNotificationFragmentDoc}
${i.NotificationItemRepliedToCommentNotificationFragmentDoc}
${i.NotificationItemAnswerAcceptedNotificationFragmentDoc}
${i.NotificationItemMultiplayerInvitedNotificationFragmentDoc}
${i.NotificationItemMultiplayerJoinedEmailNotificationFragmentDoc}
${i.NotificationItemMultiplayerJoinedLinkNotificationFragmentDoc}
${i.NotificationItemMultiplayerOverlimitNotificationFragmentDoc}
${i.NotificationItemWarningNotificationFragmentDoc}
${i.NotificationItemTeamInviteNotificationFragmentDoc}
${i.NotificationItemTeamOrganizationInviteNotificationFragmentDoc}
${i.NotificationTeamTemplateSubmittedNotificationFragmentDoc}
${i.NotificationTeamTemplateReviewedStatusNotificationFragmentDoc}
${i.NotificationReplCommentCreatedNotificationFragmentDoc}
${i.NotificationReplCommentReplyCreatedNotificationFragmentDoc}
${i.NotificationReplCommentMentionNotificationFragmentDoc}
${i.NotificationItemNewFollowerFragmentDoc}
${i.NotificationItemOrgUpgradeRequestReviewedNotificationFragmentDoc}
${i.NotificationItemEgressLimitNotificationFragmentDoc}
${i.NotificationItemIntegrationRequestedNotificationFragmentDoc}
${i.NotificationItemIntegrationRequestReviewedNotificationFragmentDoc}`,r=t.gql`
    query notifications($after: String, $count: Int, $seen: Boolean) {
  currentUser {
    id
  }
  notifications(after: $after, count: $count, seen: $seen) {
    items {
      ...NotificationItems
    }
    pageInfo {
      nextCursor
    }
  }
}
    ${n}`;e.s(["useNotificationsQuery",0,function(e){let t={...o,...e};return a.useQuery(r,t)}])},286093,e=>{"use strict";var t=e.i(276385),i=e.i(389959),a=e.i(237090),o=e.i(882708),n=e.i(183035),r=e.i(269848);e.i(214847);var s=e.i(864300),l=e.i(378099),c=e.i(137796),u=e.i(980224),d=e.i(27923),m=e.i(521299),f=e.i(643484),p=e.i(197798),g=e.i(8047),h=e.i(61732);e.s(["default",0,e=>{let v=(0,s.useIntl)(),[x,y]=(0,i.useState)(!0),{count:_}=(0,u.default)(),[w,b]=(0,i.useState)(!1),{data:C,loading:N,fetchMore:M}=(0,a.useNotificationsQuery)({fetchPolicy:"cache-and-network",ssr:!1,notifyOnNetworkStatusChange:!0,variables:{...e.count?{count:e.count}:{},...x?{seen:!1}:{}}}),I=(C?.notifications?.items??[]).filter(e=>"AnnotationNotification"!==e.__typename&&"ThreadNotification"!==e.__typename),[j]=(0,o.useMarkAllNotificationsAsSeenMutation)({onCompleted({markAllNotificationsAsSeen:e}){b(!1),window.replitDesktop?.setNotificationBadgeCount?.(e.notificationCount)},optimisticResponse:{__typename:"RootMutationType",markAllNotificationsAsSeen:{__typename:"CurrentUser",id:C?.currentUser?.id,notificationCount:0}}}),R=(0,m.useIdSeed)();return(0,t.jsxs)(t.Fragment,{children:[(0,t.jsxs)(h.View,{row:!0,gap:16,justify:"space-between",clsx:(0,d.tw)(!0===e.compact?(0,d.tw)("border-b p-300",d.tw.designSystemDeviation("border-(--outline-dimmest)")):"pb-400"),children:[(0,t.jsx)("div",{clsx:(0,d.tw)(!0===e.compact&&_>0?d.tw.designSystemDeviation("w-[160px] max-w-[200px]"):"w-full"),children:(0,t.jsxs)(p.ButtonGroup2,{name:R("visibility"),value:x.toString(),onChange:()=>y(!x),row:!0,stretch:!0,children:[(0,t.jsx)(p.ButtonGroup2Item,{id:R("true"),value:"true",text:v.formatMessage({id:"notifications.unread",defaultMessage:"Unread"})}),(0,t.jsx)(p.ButtonGroup2Item,{id:R("false"),value:"false",text:v.formatMessage({id:"notifications.all",defaultMessage:"All"})})]})}),_>0&&(0,t.jsx)(f.Button,{text:w?v.formatMessage({id:"notifications.markingAll",defaultMessage:"Marking all..."}):v.formatMessage({id:"notifications.markAsRead",defaultMessage:"Mark as read"}),disabled:N,iconLeft:(0,t.jsx)(n.default,{}),stretch:!0,onClick:()=>{b(!0),j()}})]}),(0,t.jsxs)(h.View,{children:[!N||C&&C.notifications?null:(0,t.jsx)(h.View,{p:64,align:"center",children:(0,t.jsx)(r.default,{})}),0===I.length&&(0,t.jsx)("div",{clsx:(0,d.tw)(!0===e.compact&&"p-300"),children:(0,t.jsx)(h.View,{clsx:(0,d.tw)("flex w-full items-center justify-center py-800 px-300",!0===e.compact?"bg-transparent":d.tw.designSystemDeviation("bg-(--background-default)"),d.tw.designSystemDeviation("rounded-(--border-radius-8)")),children:(0,t.jsx)(g.Text,{variant:"text",color:"dimmer",multiline:!1,clsx:(0,d.tw)("text-center"),children:x?v.formatMessage({id:"notifications.allCaughtUp",defaultMessage:"You're all caught up!"}):v.formatMessage({id:"notifications.noNotifications",defaultMessage:"No notifications"})})})}),I.length?(0,t.jsxs)(t.Fragment,{children:[I.map((i,a)=>(0,t.jsx)(l.default,{compact:e.compact,notification:i,isLastItem:a===I.length-1,setAsSeen:!0},i.id)),e.markAsSeen&&(0,t.jsx)(c.default,{notificationIds:I.filter(e=>"seen"in e&&!e.seen).map(e=>e.id)})]}):null,e.loadMore&&C?.notifications.pageInfo.nextCursor?(0,t.jsx)("div",{clsx:(0,d.tw)("py-300",!0===e.compact&&"px-300"),children:(0,t.jsx)(f.Button,{text:N?v.formatMessage({id:"notifications.loading",defaultMessage:"Loading..."}):v.formatMessage({id:"notifications.loadMore",defaultMessage:"Load more"}),onClick:()=>{N||M({variables:{after:C&&C.notifications&&!x?C.notifications.pageInfo.nextCursor:null},updateQuery:(e,t)=>{if(!t||!t.fetchMoreResult)return e;let{fetchMoreResult:i}=t,a=e?e.notifications.items:[],o={...i};return o.notifications.items=[...a,...i.notifications.items],o}})},disabled:N})}):null]})]})}])},882708,e=>{"use strict";var t=e.i(351623),i=e.i(299020);let a={},o=t.gql`
    mutation MarkAllNotificationsAsSeen {
  markAllNotificationsAsSeen {
    id
    notificationCount
  }
}
    `;e.s(["useMarkAllNotificationsAsSeenMutation",0,function(e){let t={...a,...e};return i.useMutation(o,t)}])},137796,e=>{"use strict";var t=e.i(389959),i=e.i(295819),a=e.i(980224);e.s(["default",0,e=>{let[o,n]=(0,t.useState)([]),{count:r,setUnreadCount:s}=(0,a.default)(),l=e.notificationIds.filter(e=>-1===o.indexOf(e)),[c,{data:u}]=(0,i.useMarkNotificationsAsSeenMutation)({variables:{ids:l}});return(0,t.useEffect)(()=>{0!==l.length&&(n([...o,...l]),c({variables:{ids:l}}))},[l,c]),(0,t.useEffect)(()=>{if(u?.markNotificationsAsSeen){let e=Math.max(0,r-u.markNotificationsAsSeen);s(e),window.replitDesktop?.setNotificationBadgeCount?.(e)}},[u,s]),null}])},795968,e=>{e.v({content:"NotificationCard-module__5qO-ya__content",indicator:"NotificationCard-module__5qO-ya__indicator",indicatorLink:"NotificationCard-module__5qO-ya__indicatorLink",notificationLink:"NotificationCard-module__5qO-ya__notificationLink",notificationLinkWrapper:"NotificationCard-module__5qO-ya__notificationLinkWrapper",root:"NotificationCard-module__5qO-ya__root"})},377498,e=>{"use strict";var t=e.i(276385),i=e.i(36454),a=e.i(389959),o=e.i(927600),n=e.i(50703),r=e.i(415541),s=e.i(89148),l=e.i(795968);let c=({condition:e,children:t,wrap:i})=>e?(0,a.cloneElement)(i(t)):t;e.s(["default",0,function({children:e,seen:a,href:u,as:d,isLastItem:m=!1}){let f=!!(u||d);return(0,t.jsx)(c,{condition:f,wrap:e=>(0,t.jsxs)("div",{clsx:l.default.notificationLinkWrapper,children:[d&&u?(0,t.jsx)(i.default,{as:d,href:u,clsx:l.default.notificationLink,children:e}):null,!d&&u&&"object"==typeof u?(0,t.jsx)(i.default,{href:u,clsx:l.default.notificationLink,children:e}):null,d||"string"!=typeof u?null:(0,t.jsx)("a",{clsx:l.default.notificationLink,href:u,children:e})]}),children:(0,t.jsxs)("div",{onClick:()=>{f&&(0,r.track)(n.clientEvents.NOTIFICATION_ITEM_CLICKED,{seen:a})},clsx:l.default.root,"data-has-link":f,"data-last-item":m,children:[(0,t.jsx)("div",{clsx:l.default.content,children:e}),f?(0,t.jsxs)("div",{clsx:l.default.indicatorLink,children:[!a&&(0,t.jsx)("div",{clsx:l.default.indicator}),(0,t.jsx)(o.default,{color:s.tokens.foregroundDimmest})]}):null]})})}])},151155,e=>{e.v({pageDot:"CoreToProUpsellModal-module__b1IHQG__pageDot",pageDotActive:"CoreToProUpsellModal-module__b1IHQG__pageDotActive"})},743884,e=>{"use strict";var t=e.i(276385),i=e.i(121758),a=e.i(596139);e.i(214847);var o=e.i(864300),n=e.i(872862),r=e.i(643484),s=e.i(528326),l=e.i(8047),c=e.i(61732),u=e.i(92142),d=e.i(338851),m=e.i(248874),f=e.i(151155);function p({isOpen:e,onDismiss:i,onNext:n,audience:m,isNextLoading:g=!1,portalContainer:h}){let v=(0,o.useIntl)(),x=v.formatMessage({id:"onboarding.coreToProUpsellTitle",defaultMessage:"You're ready for Replit {proPlanName}"},{proPlanName:a.proPlanName}),y=m&&"heavy_spender"!==m?v.formatMessage({id:"onboarding.coreToProUpsellBodyLeadRole",defaultMessage:"{proPlanName} is built for {audience, select, startup_founder {founders} business_owner {business owners} product_manager {product managers} marketing_sales {marketers} business_ops {operators} other {teams and creators}} like you who are ready to move faster without limits."},{audience:m,proPlanName:a.proPlanName}):v.formatMessage({id:"onboarding.coreToProUpsellBodyLead",defaultMessage:"{proPlanName} is built for teams and creators like you who are ready to move faster without limits."},{proPlanName:a.proPlanName});return(0,t.jsx)(s.Modal,{isOpen:e,onRequestClose:i,portalContainer:h,maxWidth:440,centered:!0,preventOutsideModalClickClose:!0,hideCloseButton:g,ariaLabel:x,dataAnalyticsId:"core_to_pro_upsell_modal",children:(0,t.jsxs)(c.View,{gap:20,pt:16,children:[(0,t.jsx)(u.ProPlanHero,{}),(0,t.jsxs)(c.View,{gap:8,children:[(0,t.jsx)(l.Text,{color:"dimmer",children:v.formatMessage({id:"onboarding.coreToProUpsellEyebrow",defaultMessage:"Recommended for you"})}),(0,t.jsx)(l.Text,{variant:"headerDefault",children:x}),(0,t.jsxs)(l.Text,{color:"dimmest",children:[y," ",v.formatMessage({id:"onboarding.coreToProUpsellBodyBenefits",defaultMessage:"Get more credits at a better rate, 10 parallel agents, priority support, and much more on {proPlanName}."},{proPlanName:a.proPlanName})]})]}),(0,t.jsxs)(c.View,{row:!0,align:"center",justify:"space-between",children:[(0,t.jsxs)(c.View,{row:!0,gap:6,"aria-hidden":!0,children:[(0,t.jsx)("span",{clsx:[f.default.pageDot,f.default.pageDotActive]}),(0,t.jsx)("span",{clsx:f.default.pageDot})]}),(0,t.jsx)(r.Button,{text:v.formatMessage({id:"onboarding.coreToProUpsellNext",defaultMessage:"Explore Pro"}),iconLeft:(0,t.jsx)(d.ProPlanIcon,{compact:!0}),colorway:"primary",size:"big",loading:g,onClick:n,"data-analytics-id":"core_to_pro_upsell_next_button"})]})]})})}function g({isOpen:e,audience:i,onDismiss:a,onUpgradeComplete:o,portalContainer:n}){let{isLoading:r,open:s}=(0,m.useCoreToProUpgradeModal)("upsell_modal",i??void 0),l=async()=>{await a(),await s()&&o?.()};return(0,t.jsx)(p,{isOpen:e,audience:i,onDismiss:a,onNext:l,isNextLoading:r,portalContainer:n})}let h=(0,n.createNiceModalWithTrackingHierarchy)(function({onDismiss:e,onUpgradeComplete:a,portalContainer:o}){let n=(0,i.useModal)();return(0,t.jsx)(g,{isOpen:n.visible,onDismiss:()=>{e?.(),n.hide()},onUpgradeComplete:a,portalContainer:o})});e.s(["CoreToProUpsellModal",0,p,"CoreToProUpsellModalFlow",0,g,"default",0,h])},894962,e=>{e.v({amountLabel:"TopUpsAutoReloadCard-module__xSi9qG__amountLabel",amountRow:"TopUpsAutoReloadCard-module__xSi9qG__amountRow",amountRowDisabled:"TopUpsAutoReloadCard-module__xSi9qG__amountRowDisabled",card:"TopUpsAutoReloadCard-module__xSi9qG__card",divider:"TopUpsAutoReloadCard-module__xSi9qG__divider",label:"TopUpsAutoReloadCard-module__xSi9qG__label",select:"TopUpsAutoReloadCard-module__xSi9qG__select",selectControl:"TopUpsAutoReloadCard-module__xSi9qG__selectControl"})},922757,e=>{"use strict";var t=e.i(276385),i=e.i(761201);e.i(214847);var a=e.i(614852),o=e.i(20397),n=e.i(864300),r=e.i(89148),s=e.i(643484),l=e.i(39114),c=e.i(19322),u=e.i(327391),d=e.i(8047),m=e.i(61732),f=e.i(894962);let p=e=>(0,a.formatCentsAsCurrency)(e,{maximumFractionDigits:0}),g="no-limit",h=e=>null===e?g:String(e),v=e=>(0,t.jsx)("a",{href:i.LINKS_LEGAL.TERMS_OF_SERVICE,target:"_blank",rel:"noreferrer",children:e});function x(){return(0,t.jsx)(d.Text,{variant:"small",color:"dimmest",children:(0,t.jsx)(o.FormattedMessage,{id:"onboarding.upgradeWelcomeTopUpsTerms",defaultMessage:"Taxes may apply. By continuing you agree to our <terms>terms</terms>.",values:{terms:v}})})}function y({label:e,options:i,value:a,onChange:o,isDisabled:r}){let s=(0,n.useIntl)(),u=e=>null===e?s.formatMessage({id:"onboarding.upgradeWelcomeTopUpsNoLimit",defaultMessage:"No limit"}):p(e);return(0,t.jsxs)(m.View,{clsx:[f.default.amountRow,r&&f.default.amountRowDisabled],row:!0,align:"center",justify:"space-between",gap:12,children:[(0,t.jsx)(d.Text,{clsx:f.default.amountLabel,variant:"text",children:e}),(0,t.jsx)(m.View,{clsx:f.default.select,shrink:0,children:(0,t.jsx)(c.Select,{className:f.default.selectControl,"aria-label":e,isDisabled:r,selectedKey:h(a),onSelectionChange:e=>o(e===g?null:Number(e)),selectValue:()=>(0,t.jsx)(d.Text,{variant:"small",children:u(a)}),children:i.map(e=>(0,t.jsx)(l.BaseListBoxItem,{id:h(e),textValue:u(e),children:(0,t.jsx)(d.Text,{variant:"small",children:u(e)})},h(e)))})})]})}e.s(["TopUpsAutoReloadCard",0,function({enabled:e,onEnabledChange:i,reloadAmount:a,onReloadAmountChange:o,reloadAmountOptions:l,threshold:c,onThresholdChange:g,monthlyLimit:h,onMonthlyLimitChange:v,thresholdOptions:_,limitOptions:w,errorMessage:b,hasErrored:C,isSaving:N,onSetUpLater:M,hideSettingsWhenDisabled:I=!1,showSetUpLater:j=!0,showTerms:R=!0}){let T=(0,n.useIntl)(),S=T.formatMessage({id:"onboarding.upgradeWelcomeTopUpsAutoReloadLabel",defaultMessage:"Auto-reload enabled"}),k=T.formatMessage({id:"onboarding.upgradeWelcomeTopUpsAutoReloadDisabledLabel",defaultMessage:"Auto-reload disabled"}),A=e?S:k,P=e?T.formatMessage({id:"onboarding.upgradeWelcomeTopUpsAutoReloadSublabel",defaultMessage:"Automatically reload {amount} when your credits are lower than {threshold}"},{amount:p(a),threshold:p(c)}):T.formatMessage({id:"onboarding.upgradeWelcomeTopUpsAutoReloadDisabledSublabel",defaultMessage:"Turn on to top up your credits automatically."});return(0,t.jsxs)(m.View,{clsx:f.default.card,br:"container",children:[(0,t.jsxs)(m.View,{row:!0,align:"center",justify:"space-between",gap:12,p:12,children:[(0,t.jsxs)(m.View,{gap:2,shrink:!0,children:[(0,t.jsx)(m.View,{clsx:f.default.label,children:(0,t.jsx)(d.Text,{variant:"text",children:A})}),(0,t.jsx)(d.Text,{variant:"small",color:"dimmest",children:P})]}),(0,t.jsx)(u.Switch,{isSelected:e,onChange:i,"aria-label":S})]}),e||!I?(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(m.View,{clsx:f.default.divider}),(0,t.jsxs)(m.View,{gap:6,p:12,children:[(0,t.jsx)(y,{label:T.formatMessage({id:"onboarding.upgradeWelcomeTopUpsReloadAmountLabel",defaultMessage:"Reload amount"}),options:l,value:a,onChange:o,isDisabled:!e}),(0,t.jsx)(y,{label:T.formatMessage({id:"onboarding.upgradeWelcomeTopUpsThresholdLabel",defaultMessage:"Minimum credit balance to trigger reload"}),options:_,value:c,onChange:g,isDisabled:!e}),(0,t.jsx)(y,{label:T.formatMessage({id:"onboarding.upgradeWelcomeTopUpsMonthlyLimitLabel",defaultMessage:"Monthly reload limit"}),options:w,value:h,onChange:v,isDisabled:!e}),e&&R?(0,t.jsx)(x,{}):null]})]}):null,C?(0,t.jsxs)(m.View,{px:12,pb:12,row:!0,wrap:!0,align:"baseline",gap:4,children:[(0,t.jsx)(d.Text,{variant:"small",style:{color:r.tokens.redStronger},children:b}),j?(0,t.jsx)(s.Button,{variant:"underlinedOnHover",size:"small",disabled:N,"data-analytics-id":"top_ups_set_up_later_button",text:T.formatMessage({id:"onboarding.upgradeWelcomeTopUpsSetUpLater",defaultMessage:"Set up later"}),onClick:M}):null]}):null]})},"TopUpsTerms",0,x])},874071,e=>{"use strict";var t=e.i(389959),i=e.i(912206);e.s(["useImmediateChargeIdempotencyKey",0,function(){let e=(0,t.useRef)(null);return t=>{let a=e.current;if(a?.amountCents===t.amountCents)return a.idempotencyKey;let o=(0,i.v4)();return e.current={amountCents:t.amountCents,idempotencyKey:o},o}}])},388755,e=>{"use strict";var t=e.i(389959),i=e.i(476652);e.s(["useTopUpsAutoReloadForm",0,function({defaultReloadAmountCents:e=i.DEFAULT_TOP_UP_AMOUNT_CENTS,initialConfig:a,initialEnabled:o=!0,proAutoTopUpOptions:n}={}){let r=n?.amountOptionsCents??i.TOP_UP_AMOUNTS_CENTS,s=n?.thresholdOptionsCents??i.TOP_UP_THRESHOLDS_CENTS,l=n?.limitOptionsCents??i.TOP_UP_LIMITS_CENTS,c=a?.amountCents??n?.defaultAmountCents??e,u=a?.thresholdCents??n?.defaultThresholdCents??i.DEFAULT_TOP_UP_THRESHOLD_CENTS,d=a?a.limitCents:n?.defaultLimitCents??i.DEFAULT_TOP_UP_LIMIT_CENTS,[m,f]=(0,t.useState)(o),[p,g]=(0,t.useState)(c),[h,v]=(0,t.useState)(u),[x,y]=(0,t.useState)(d),_=l.filter(e=>(0,i.isTopUpLimitGteAmount)(e,p)),w=s.filter(e=>(0,i.isTopUpThresholdBelowAmount)(e,p));return{config:{amountCents:p,thresholdCents:h,limitCents:x},cardProps:{enabled:m,onEnabledChange:f,reloadAmount:p,onReloadAmountChange:e=>{null!==e&&(g(e),y(t=>(0,i.isTopUpLimitGteAmount)(t,e)?t:l.find(t=>null!==t&&t>=e)??null),v(t=>(0,i.isTopUpThresholdBelowAmount)(t,e)?t:[...s].reverse().find(t=>t<e)??u))},reloadAmountOptions:r,threshold:h,onThresholdChange:e=>null!==e&&v(e),monthlyLimit:x,onMonthlyLimitChange:y,thresholdOptions:w,limitOptions:_}}}])},827320,e=>{"use strict";var t=e.i(276385),i=e.i(389959),a=e.i(27923),o=e.i(8047);e.s(["Prose",0,({children:e,className:n,...r})=>(0,t.jsx)(o.Text,{multiline:!0,clsx:a.tw.merge(a.tw.onCustom("[&_ul]")((0,a.tw)("ml-400 py-100",a.tw.designSystemDeviation("list-[initial]"))),a.tw.onCustom("[&_ol]")("ml-400 py-100"),a.tw.onCustom("[&_a]")((0,a.tw)("cursor-pointer pointer-events-auto no-underline",a.tw.designSystemDeviation("text-(--accent-primary-stronger)"))),a.tw.onCustom("[&_a:hover]")("underline"),a.tw.onCustom("[&_a:focus-visible]")("underline"),a.tw.onCustom("[&_h1]")("block mb-200"),a.tw.onCustom("[&_h2]")("block mb-200"),a.tw.onCustom("[&_h3]")("block mb-200"),a.tw.onCustom("[&_*+h1]")("mt-200"),a.tw.onCustom("[&_*+h2]")("mt-200"),a.tw.onCustom("[&_*+h3]")("mt-200"),a.tw.external(n)),...r,children:i.Children.map(e,e=>"string"==typeof e?(0,t.jsx)("span",{children:e}):e)})])},124298,e=>{"use strict";var t=e.i(276385),i=e.i(602686),a=e.i(983420);e.i(459890);var o=e.i(500355),n=e.i(547523);e.i(214847);var r=e.i(864300),s=e.i(27923),l=e.i(919073),c=e.i(643484),u=e.i(419635),d=e.i(488299),m=e.i(8047),f=e.i(61732);e.s(["TopBanner",0,({buttonAnalyticsId:e,buttonAnalyticsDestination:p,dataAnalyticsId:g,dismissButtonAnalyticsId:h,innerRef:v,text:x,iconLeft:y,buttonProps:_,buttonLinkProps:w,colorway:b,onDismiss:C,mobileLayout:N="stacked",children:M})=>{let I=(0,r.useIntl)(),j=(0,n.useBreakpoint)("mobileMax"),R=j&&"stacked"===N,T=j?"small":"default",S=null;_?S=(0,t.jsx)(c.Button,{"data-analytics-id":e,"data-analytics-destination":p,..._,size:_.size??T}):w&&(S=(0,t.jsx)(u.ButtonLink,{"data-analytics-id":e,"data-analytics-destination":p,...w,size:w.size??T}));let k=C?(0,t.jsx)(d.IconButton,{colorway:b,"data-analytics-id":h,alt:I.formatMessage({id:"rui.topBannerDismiss",defaultMessage:"Dismiss"}),onClick:C,children:(0,t.jsx)(i.default,{})}):null;return(0,t.jsx)(l.ShadesSurface,{"data-analytics-id":g,border:j?{side:["top","bottom"],strength:"subtle"}:"subtle",colorShade:b,clsx:(0,s.tw)("shrink justify-center rounded-full py-100 px-300",s.tw.on("max-mobile-max")("grow rounded-none py-100 px-150")),elevate:"1x",innerRef:v,children:(0,t.jsxs)(f.View,{clsx:s.tw.merge("relative flex flex-row items-center justify-between gap-200 w-full min-w-0",s.tw.external(o.classes.text)),children:[(0,t.jsxs)(f.View,{clsx:(0,s.tw)("flex flex-1 flex-row items-center justify-center gap-200 min-w-0","row"===N?s.tw.on("max-mobile-max")("flex-row gap-200 py-0 px-50"):s.tw.on("max-mobile-max")("flex-col gap-100 py-50 px-200")),children:[(0,t.jsxs)(f.View,{clsx:(0,s.tw)("flex flex-row grow shrink items-center","row"===N?"justify-start gap-200":"justify-center gap-100"),children:[y&&!R?(0,t.jsx)(f.View,{clsx:(0,s.tw)("shrink-0"),children:(0,t.jsx)(a.IconProvider,{size:16,children:y})}):null,(0,t.jsx)(m.Text,{multiline:R,showTooltipOnTruncate:!R,textAlign:R?"center":void 0,textWrap:R?"balance":void 0,shrink:!0,children:x})]}),M,S?(0,t.jsx)(f.View,{clsx:(0,s.tw)("flex flex-row shrink-0 items-center gap-100"),children:S}):null]}),k?(0,t.jsx)(f.View,{clsx:(0,s.tw)("shrink-0",s.tw.on("max-mobile-max")((0,s.tw)("top-0 right-0","row"===N?"static":"absolute"))),children:k}):null]})})}])},739980,e=>{"use strict";var t=e.i(276385),i=e.i(810047);e.s(["HackFontLoader",0,()=>(0,t.jsx)(i.default,{children:(0,t.jsx)("style",{dangerouslySetInnerHTML:{__html:`
        @font-face {
          font-family: 'ReplitHack';
          src: url('/public/fonts/hack-regular.woff2?sha=3114f1256') format('woff2'), url('/public/fonts/hack-regular.woff?sha=3114f1256') format('woff');
          font-weight: 400;
          font-style: normal;
        }
        
        @font-face {
          font-family: 'ReplitHack';
          src: url('/public/fonts/hack-bold.woff2?sha=3114f1256') format('woff2'), url('/public/fonts/hack-bold.woff?sha=3114f1256') format('woff');
          font-weight: 700;
          font-style: normal;
        }
        
        @font-face {
          font-family: 'ReplitHack';
          src: url('/public/fonts/hack-italic.woff2?sha=3114f1256') format('woff2'), url('/public/fonts/hack-italic.woff?sha=3114f1256') format('woff');
          font-weight: 400;
          font-style: italic;
        }
        
        @font-face {
          font-family: 'ReplitHack';
          src: url('/public/fonts/hack-bolditalic.woff2?sha=3114f1256') format('woff2'), url('/public/fonts/hack-bolditalic.woff?sha=3114f1256') format('woff');
          font-weight: 700;
          font-style: italic;
        }
        `}})})])},192915,e=>{"use strict";var t=e.i(276385),i=e.i(36454);let a=e=>({href:{pathname:"/profile",query:{username:"string"==typeof e?e:e.username}},as:"string"==typeof e?`/@${e}`:e.url});e.s(["UserLink",0,({user:e,children:o})=>(0,t.jsx)(i.default,{...a(e),prefetch:!1,children:o}),"userLinkProps",0,a])},443505,e=>{"use strict";var t=e.i(389959),i=e.i(753451),a=e.i(584878);e.s(["default",0,function({onChange:e}){let o=(0,i.useIsInBonsaiWebview)();(0,t.useEffect)(()=>{if(!o)return;let t=t=>(0,a.payingStatusChangedBridgeMessageHandler)(t,()=>{e()});return window.addEventListener("message",t),()=>{window.removeEventListener("message",t)}},[o,e])}])},638141,e=>{"use strict";var t=e.i(15801),i=e.i(389959),a=e.i(179104),o=e.i(582168),n=e.i(753451),r=e.i(584878);e.s(["default",0,function(){let e=(0,t.useRouter)(),s=(0,n.doesBonsaiWebviewSupportFeature)(e,"stripePayment")&&(0,n.isInBonsaiWebview)(e);return{showPaymentFlow:(0,i.useCallback)(e=>{let t={messageType:o.BridgeMessageType.SHOW_PAYMENT_FLOW,flow:e};switch(e.type){case"setup":if(!s)break;(0,r.sendMessage)(t,(0,a.nanoid)());break;case"setUsageLimits":(0,r.sendMessage)(t,(0,a.nanoid)())}},[s])}}])},519979,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{d:"M9 1.25a1 1 0 0 1 .902.57l.058.15.002.008L15 19.9l1.868-6.643a2.75 2.75 0 0 1 2.652-2.007H22a.75.75 0 0 1 0 1.5h-2.481a1.25 1.25 0 0 0-1.206.912l-2.351 8.361-.002.007a1.001 1.001 0 0 1-1.92 0l-.002-.008L9 4.1l-1.867 6.644a2.75 2.75 0 0 1-2.64 2.007H2a.75.75 0 0 1 0-1.5h2.488a1.25 1.25 0 0 0 1.2-.913l2.35-8.36.002-.007a1 1 0 0 1 .36-.52l.136-.086A1 1 0 0 1 9 1.25"})})}])},648880,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(i.default,{...e,children:[(0,t.jsx)("path",{d:"M13.083 20.625a.75.75 0 0 1 1.298.75 2.75 2.75 0 0 1-4.762 0 .75.75 0 0 1 1.299-.75 1.25 1.25 0 0 0 2.165 0"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 1.25A6.75 6.75 0 0 1 18.75 8c0 2.17.34 3.544.8 4.517s1.063 1.602 1.728 2.288l.016.017a1.749 1.749 0 0 1-1.168 2.923L20 17.75H4a1.752 1.752 0 0 1-1.291-2.93l.015-.016c.664-.686 1.268-1.315 1.728-2.288.43-.912.756-2.175.795-4.119L5.249 8A6.75 6.75 0 0 1 12 1.25m0 1.5A5.25 5.25 0 0 0 6.75 8c0 2.328-.366 3.933-.944 5.156-.572 1.212-1.331 1.995-1.99 2.675l-.04.06a.25.25 0 0 0 .225.36h16a.25.25 0 0 0 .228-.15.25.25 0 0 0-.044-.269c-.66-.68-1.42-1.463-1.992-2.675-.578-1.223-.944-2.83-.944-5.157A5.25 5.25 0 0 0 12 2.75",clipRule:"evenodd"})]})}])},166970,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M9 2.75a.25.25 0 0 0-.25.25v2c0 .138.112.25.25.25h6a.25.25 0 0 0 .25-.25V3a.25.25 0 0 0-.25-.25zm7.75.5V3A1.75 1.75 0 0 0 15 1.25H9A1.75 1.75 0 0 0 7.25 3v.25H6A2.75 2.75 0 0 0 3.25 6v14A2.75 2.75 0 0 0 6 22.75h12A2.75 2.75 0 0 0 20.75 20V6A2.75 2.75 0 0 0 18 3.25zm0 1.5V5A1.75 1.75 0 0 1 15 6.75H9A1.75 1.75 0 0 1 7.25 5v-.25H6A1.25 1.25 0 0 0 4.75 6v14A1.25 1.25 0 0 0 6 21.25h12A1.25 1.25 0 0 0 19.25 20V6A1.25 1.25 0 0 0 18 4.75z",clipRule:"evenodd"})})}])},98346,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M2.25 6A3.75 3.75 0 0 1 6 2.25h10.5A3.75 3.75 0 0 1 20.25 6v3a.75.75 0 0 1-1.5 0V6a2.25 2.25 0 0 0-2.25-2.25H6A2.25 2.25 0 0 0 3.75 6v12A2.25 2.25 0 0 0 6 20.25h10.5A2.25 2.25 0 0 0 18.75 18v-1.979a.75.75 0 0 1 1.5 0V18a3.75 3.75 0 0 1-3.75 3.75H6A3.75 3.75 0 0 1 2.25 18zm12.124 1.677a.75.75 0 0 1 0 1.06L11.41 11.7h9.59a.75.75 0 0 1 0 1.5h-9.59l2.963 2.962a.75.75 0 1 1-1.061 1.06L9.07 12.98a.75.75 0 0 1 0-1.06l4.243-4.243a.75.75 0 0 1 1.06 0",clipRule:"evenodd"})})}])},759317,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M11.573 2.263c.5.013.874.325 1.05.693.18.378.179.857-.073 1.26l-.001-.002a5.252 5.252 0 0 0 7.235 7.235c.401-.25.88-.254 1.26-.073.392.187.72.601.69 1.151A9.75 9.75 0 1 1 11.471 2.265zm-.518 1.543a8.25 8.25 0 1 0 3.894 15.899 8.25 8.25 0 0 0 5.243-6.762 6.748 6.748 0 0 1-9.137-9.137",clipRule:"evenodd"})})}])},222878,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{d:"M12.213 16.012a.5.5 0 0 1-.5.5h-.568a.5.5 0 0 1-.5-.5v-2.726c0-.809.647-1.452 1.44-1.61 2.387-.475 3.74-1.94 3.74-4.04v-.532c0-2.1-1.456-3.528-3.78-3.528-2.203 0-3.539 1.172-4.18 2.98a.53.53 0 0 1-.671.337l-.53-.19a.48.48 0 0 1-.3-.606c.813-2.295 2.632-3.977 5.737-3.977 3.36 0 5.46 2.044 5.46 5.18 0 3.274-2.201 5.106-4.92 5.64a.52.52 0 0 0-.428.505zm-.784 6.24c-.896 0-1.316-.476-1.316-1.26v-.308c0-.784.42-1.26 1.316-1.26.924 0 1.344.476 1.344 1.26v.308c0 .784-.42 1.26-1.344 1.26"})})}])},308521,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(i.default,{...e,children:[(0,t.jsx)("path",{d:"M12 19.25a.75.75 0 0 1 .75.75v2a.75.75 0 0 1-1.5 0v-2a.75.75 0 0 1 .75-.75M5.81 17.13a.75.75 0 0 1 1.06 1.06L5.46 19.6a.75.75 0 0 1-1.06-1.06zM17.13 17.13a.75.75 0 0 1 1.06 0l1.41 1.41a.75.75 0 0 1-1.06 1.06l-1.41-1.41a.75.75 0 0 1 0-1.06"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 7.25a4.75 4.75 0 1 1 0 9.5 4.75 4.75 0 0 1 0-9.5m0 1.5a3.25 3.25 0 1 0 0 6.5 3.25 3.25 0 0 0 0-6.5",clipRule:"evenodd"}),(0,t.jsx)("path",{d:"M4 11.25a.75.75 0 0 1 0 1.5H2a.75.75 0 0 1 0-1.5zM22 11.25a.75.75 0 0 1 0 1.5h-2a.75.75 0 0 1 0-1.5zM4.4 4.4a.75.75 0 0 1 1.06 0l1.41 1.41a.75.75 0 0 1-1.06 1.06L4.4 5.46a.75.75 0 0 1 0-1.06M18.54 4.4a.75.75 0 0 1 1.06 1.06l-1.41 1.41a.75.75 0 0 1-1.06-1.06zM12 1.25a.75.75 0 0 1 .75.75v2a.75.75 0 0 1-1.5 0V2a.75.75 0 0 1 .75-.75"})]})}])}]);

//# debugId=f644aece-e850-a7e9-153e-62da8dc630c3
//# sourceMappingURL=1p6tetz8ltm2w.js.map