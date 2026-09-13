;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="07a4aa8d-f1dc-c763-3c0c-558c7b304f67")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,661832,e=>{"use strict";e.s(["rgbaToThumbHash",0,function(e,t,i){if(e>100||t>100)throw Error(`${e}x${t} doesn't fit in 100x100`);let{PI:a,round:r,max:n,cos:o,abs:s}=Math,l=0,u=0,d=0,p=0;for(let a=0,r=0;a<e*t;a++,r+=4){let e=i[r+3]/255;l+=e/255*i[r],u+=e/255*i[r+1],d+=e/255*i[r+2],p+=e}p&&(l/=p,u/=p,d/=p);let c=p<e*t,g=c?5:7,m=n(1,r(g*e/n(e,t))),A=n(1,r(g*t/n(e,t))),f=[],h=[],S=[],y=[];for(let a=0,r=0;a<e*t;a++,r+=4){let e=i[r+3]/255,t=l*(1-e)+e/255*i[r],n=u*(1-e)+e/255*i[r+1],o=d*(1-e)+e/255*i[r+2];f[a]=(t+n+o)/3,h[a]=(t+n)/2-o,S[a]=t-n,y[a]=e}let I=(i,r,l)=>{let u=0,d=[],p=0,c=[];for(let g=0;g<l;g++)for(let m=0;m*l<r*(l-g);m++){let r=0;for(let t=0;t<e;t++)c[t]=o(a/e*m*(t+.5));for(let n=0;n<t;n++)for(let s=0,l=o(a/t*g*(n+.5));s<e;s++)r+=i[s+n*e]*c[s]*l;r/=e*t,m||g?(d.push(r),p=n(p,s(r))):u=r}if(p)for(let e=0;e<d.length;e++)d[e]=.5+.5/p*d[e];return[u,d,p]},[_,C,B]=I(f,n(3,m),n(3,A)),[k,P,N]=I(h,3,3),[U,T,R]=I(S,3,3),[v,z,b]=c?I(y,5,5):[],E=e>t,M=r(63*_)|r(31.5+31.5*k)<<6|r(31.5+31.5*U)<<12|r(31*B)<<18|c<<23,D=(E?A:m)|r(63*N)<<3|r(63*R)<<9|E<<15,O=[255&M,M>>8&255,M>>16,255&D,D>>8],w=c?6:5,L=0;for(let e of(c&&O.push(r(15*v)|r(15*b)<<4),c?[C,P,T,z]:[C,P,T]))for(let t of e)O[w+(L>>1)]|=r(15*t)<<((1&L++)<<2);return new Uint8Array(O)},"thumbHashToDataURL",0,function(e){let t=function(e){var t;let i,a,r,{PI:n,min:o,max:s,cos:l,round:u}=Math,d=e[0]|e[1]<<8|e[2]<<16,p=e[3]|e[4]<<8,c=(63&d)/63,g=(d>>6&63)/31.5-1,m=(d>>12&63)/31.5-1,A=d>>23,f=p>>15,h=s(3,f?A?5:7:7&p),S=s(3,f?7&p:A?5:7),y=A?(15&e[5])/15:1,I=(e[5]>>4)/15,_=A?6:5,C=0,B=(t,i,a)=>{let r=[];for(let n=0;n<i;n++)for(let o=+!n;o*i<t*(i-n);o++)r.push(((e[_+(C>>1)]>>((1&C++)<<2)&15)/7.5-1)*a);return r},k=B(h,S,(d>>18&31)/31),P=B(3,3,(p>>3&63)/63*1.25),N=B(3,3,(p>>9&63)/63*1.25),U=A&&B(5,5,I),T=(i=(t=e)[3],a=128&t[2],((r=128&t[4])?a?5:7:7&i)/(r?7&i:a?5:7)),R=u(T>1?32:32*T),v=u(T>1?32/T:32),z=new Uint8Array(R*v*4),b=[],E=[];for(let e=0,t=0;e<v;e++)for(let i=0;i<R;i++,t+=4){let a=c,r=g,u=m,d=y;for(let e=0,t=s(h,A?5:3);e<t;e++)b[e]=l(n/R*(i+.5)*e);for(let t=0,i=s(S,A?5:3);t<i;t++)E[t]=l(n/v*(e+.5)*t);for(let e=0,t=0;e<S;e++)for(let i=+!e,r=2*E[e];i*S<h*(S-e);i++,t++)a+=k[t]*b[i]*r;for(let e=0,t=0;e<3;e++)for(let i=+!e,a=2*E[e];i<3-e;i++,t++){let e=b[i]*a;r+=P[t]*e,u+=N[t]*e}if(A)for(let e=0,t=0;e<5;e++)for(let i=+!e,a=2*E[e];i<5-e;i++,t++)d+=U[t]*b[i]*a;let p=a-2/3*r,f=(3*a-p+u)/2,I=f-u;z[t]=s(0,255*o(1,f)),z[t+1]=s(0,255*o(1,I)),z[t+2]=s(0,255*o(1,p)),z[t+3]=s(0,255*o(1,d))}return{w:R,h:v,rgba:z}}(e);return function(e,t,i){let a=4*e+1,r=6+t*(5+a),n=[137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,e>>8,255&e,0,0,t>>8,255&t,8,6,0,0,0,0,0,0,0,r>>>24,r>>16&255,r>>8&255,255&r,73,68,65,84,120,1],o=[0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,-0x12477ce0,-0xff06cbc,-0x29295c18,-0x349e4c74,-0x649b3d50,-0x792c2d2c,-0x5ff51d88,-0x42420de4],s=1,l=0;for(let e=0,r=0,o=a-1;e<t;e++,o+=a-1)for(n.push(e+1<t?0:1,255&a,a>>8,255&~a,a>>8^255,0),l=(l+s)%65521;r<o;r++){let e=255&i[r];n.push(e),l=(l+(s=(s+e)%65521))%65521}for(let[e,t]of(n.push(l>>8,255&l,s>>8,255&s,0,0,0,0,0,0,0,0,73,69,78,68,174,66,96,130),[[12,29],[37,41+r]])){let i=-1;for(let a=e;a<t;a++)i^=n[a],i=(i=i>>>4^o[15&i])>>>4^o[15&i];i=~i,n[t++]=i>>>24,n[t++]=i>>16&255,n[t++]=i>>8&255,n[t++]=255&i}return"data:image/png;base64,"+btoa(String.fromCharCode(...n))}(t.w,t.h,t.rgba)}])},761320,e=>{"use strict";var t=e.i(807988);e.s(["StackBlueprint",()=>t.ReplitAiStackStackBlueprint])},359947,e=>{"use strict";var t=e.i(351623),i=e.i(344480);e.i(975473);let a={},r=t.gql`
    query CanSeeReferralCtaPersonal {
  currentUser {
    ... on CurrentUser {
      id
      customer {
        ... on Customer {
          id
          authorizations {
            enablePromotions {
              isAuthorized
              code
              message
            }
          }
        }
      }
    }
  }
}
    `,n=t.gql`
    query CanSeeReferralCtaOrg($orgId: String, $orgSlug: String) {
  getOrg(orgId: $orgId, orgSlug: $orgSlug) {
    ... on Org {
      id
      customer {
        ... on Customer {
          id
          authorizations {
            enablePromotions {
              isAuthorized
              code
              message
            }
          }
        }
      }
    }
  }
}
    `;e.s(["useCanSeeReferralCtaOrgQuery",0,function(e){let t={...a,...e};return i.useQuery(n,t)},"useCanSeeReferralCtaPersonalQuery",0,function(e){let t={...a,...e};return i.useQuery(r,t)}])},671864,e=>{"use strict";var t=e.i(359947);e.s(["useCanSeeReferralCTA",0,function(e){let i,a=(0,t.useCanSeeReferralCtaPersonalQuery)({skip:"personal"!==e.type}),r=(0,t.useCanSeeReferralCtaOrgQuery)({variables:"org"===e.type?{orgId:e.orgId,orgSlug:e.orgSlug}:{},skip:"org"!==e.type}),n=r.data?.getOrg?.__typename==="Org"&&"Customer"===r.data.getOrg.customer.__typename?r.data.getOrg.customer.authorizations.enablePromotions:null,o=a.data?.currentUser?.__typename==="CurrentUser"&&"Customer"===a.data.currentUser.customer.__typename?a.data.currentUser.customer.authorizations.enablePromotions:null,s=null;return"personal"===e.type?s=o:"org"===e.type&&(s=n),"personal"===e.type?i=a.error:"org"===e.type&&(i=r.error),{authorization:s,error:i,isAuthorized:s?.isAuthorized??!1,isLoading:"loading"===e.type||("personal"===e.type?a.loading:r.loading)}}])},683405,e=>{"use strict";var t=e.i(761320);e.i(214847);let i=(0,e.i(800686).defineMessages)({agentName:{id:"ai.agentName",defaultMessage:"Agent"},agentDescription:{id:"ai.agentDescription",defaultMessage:"Agent can make changes, review its work, and debug itself automatically."},queueName:{id:"ai.queueName",defaultMessage:"Queue"},automationsPaneName:{id:"ai.automationsPaneName",defaultMessage:"Automations"},agentNameFormal:{id:"ai.agentNameFormal",defaultMessage:"Replit Agent"},agentNameCasual:{id:"ai.agentNameCasual",defaultMessage:"the agent"},agentNameCasualCapitalized:{id:"ai.agentNameCasualCapitalized",defaultMessage:"The agent"},agent:{id:"ai.agent",defaultMessage:"agent"},slack:{id:"ai.slack",defaultMessage:"Slack"},slackAgent:{id:"ai.slackAgent",defaultMessage:"Slack agent"}}),a={[t.StackBlueprint.STACK_BLUEPRINT_FULLSTACK_JS]:{name:"Fullstack JS",displayName:"Modern web app",displayNameIntlId:"ai.stackFullstackJsDisplayName",description:"Made with React and Node.js",descriptionIntlId:"ai.stackFullstackJsDescription"},[t.StackBlueprint.STACK_BLUEPRINT_STREAMLIT]:{name:"Streamlit",displayName:"Interactive data app",displayNameIntlId:"ai.stackStreamlitDisplayName",description:"Made with Streamlit and Python",descriptionIntlId:"ai.stackStreamlitDescription"},[t.StackBlueprint.STACK_BLUEPRINT_GAMESTACK_JS]:{name:"Gamestack JS",displayName:"3D game",displayNameIntlId:"ai.stackGamestackJsDisplayName",description:"Three.js games and simulations",descriptionIntlId:"ai.stackGamestackJsDescription"},[t.StackBlueprint.STACK_BLUEPRINT_FLASK_VANILLA_JS]:{name:"Flask Vanilla JS",displayName:"Web app (Python)",displayNameIntlId:"ai.stackFlaskVanillaJsDisplayName",description:"Websites with Python backend",descriptionIntlId:"ai.stackFlaskVanillaJsDescription"},[t.StackBlueprint.STACK_BLUEPRINT_PYTHON_API]:{name:"Python API",displayName:"Backend service",displayNameIntlId:"ai.stackPythonApiDisplayName",description:"Python API for backend and data",descriptionIntlId:"ai.stackPythonApiDescription"},[t.StackBlueprint.STACK_BLUEPRINT_CUSTOM]:{name:"Custom",displayName:"Stack from existing App",displayNameIntlId:"ai.stackCustomDisplayName",description:"Bring your own template",descriptionIntlId:"ai.stackCustomDescription"},[t.StackBlueprint.STACK_BLUEPRINT_AGENT]:{name:"Agent stack",displayName:"Automation",displayNameIntlId:"ai.stackAgentDisplayName",description:"Build agents and automations on Replit",descriptionIntlId:"ai.stackAgentDescription"},[t.StackBlueprint.STACK_BLUEPRINT_EXPO]:{name:"Expo",displayName:"Mobile application",displayNameIntlId:"ai.stackExpoDisplayName",description:"Build mobile applications with Expo",descriptionIntlId:"ai.stackExpoDescription"},[t.StackBlueprint.STACK_BLUEPRINT_MOCKUP_JS]:{name:"Mockup JS",displayName:"Rapid prototype",displayNameIntlId:"ai.stackMockupJsDisplayName",description:"Visual prototype with no backend – ideal for designers and PMs",descriptionIntlId:"ai.stackMockupJsDescription"},[t.StackBlueprint.STACK_BLUEPRINT_VIDEO_JS]:{name:"Video JS",displayName:"Animation",displayNameIntlId:"ai.stackVideoJsDisplayName",description:"Create motion graphics and animated videos programmatically",descriptionIntlId:"ai.stackVideoJsDescription"},[t.StackBlueprint.STACK_BLUEPRINT_PNPM_WORKSPACE]:{name:"PNPM Workspace",displayName:"Monorepo",displayNameIntlId:"ai.stackPnpmWorkspaceDisplayName",description:"Multi-package monorepo with PNPM workspaces",descriptionIntlId:"ai.stackPnpmWorkspaceDescription"},[t.StackBlueprint.STACK_BLUEPRINT_MSFT_RAYFIN]:{name:"Microsoft Fabric App",displayName:"Microsoft Fabric app",displayNameIntlId:"ai.stackMsftRayfinDisplayName",description:"Fabric-brokered Entra SSO with MSSQL",descriptionIntlId:"ai.stackMsftRayfinDescription"},[t.StackBlueprint.STACK_BLUEPRINT_DATABRICKS_APP]:{name:"Databricks App",displayName:"Databricks app",displayNameIntlId:"ai.stackDatabricksAppDisplayName",description:"Read-only app powered by Databricks SQL",descriptionIntlId:"ai.stackDatabricksAppDescription"}};function r(e){if("Auto"!==e){if("Expert Mode"===e)return t.StackBlueprint.STACK_BLUEPRINT_BEST_EFFORT_FALLBACK;if("Stack from existing App"===e||"OrgStackTemplate"===e)return t.StackBlueprint.STACK_BLUEPRINT_CUSTOM;if("Animation"===e)return t.StackBlueprint.STACK_BLUEPRINT_VIDEO_JS;for(let i of Object.values(t.StackBlueprint).filter(e=>"number"==typeof e))if(a[i]&&a[i].displayName===e)return i}}let n="agent",o="Slack",s=`${o} ${n}`;e.s(["AGENT",0,n,"AGENT_NAME",0,"Agent","AUTOMATIONS_PANE_NAME",0,"Automations","SLACK",0,o,"SLACK_AGENT",0,s,"aiMessages",0,i,"getAgentName",0,({variant:e="casual",capitalizeFirstChar:t=!1}={})=>"formal"===e?"Replit Agent":`${t?"T":"t"}he agent`,"getStackOptionFromBlueprint",0,function(e){switch(e){case t.StackBlueprint.STACK_BLUEPRINT_NONE:return"Auto";case t.StackBlueprint.STACK_BLUEPRINT_CUSTOM:return"Stack from existing App";case t.StackBlueprint.STACK_BLUEPRINT_FULLSTACK_JS:return"Modern web app";case t.StackBlueprint.STACK_BLUEPRINT_STREAMLIT:return"Interactive data app";case t.StackBlueprint.STACK_BLUEPRINT_GAMESTACK_JS:return"3D game";case t.StackBlueprint.STACK_BLUEPRINT_AGENT:return"Automation";case t.StackBlueprint.STACK_BLUEPRINT_EXPO:return"Mobile application";case t.StackBlueprint.STACK_BLUEPRINT_BEST_EFFORT_FALLBACK:return"Expert Mode";case t.StackBlueprint.STACK_BLUEPRINT_VIDEO_JS:return"Animation";case t.StackBlueprint.STACK_BLUEPRINT_PYTHON_API:case t.StackBlueprint.STACK_BLUEPRINT_MOCKUP_JS:case t.StackBlueprint.STACK_BLUEPRINT_PNPM_WORKSPACE:default:return}},"resolveStackBlueprintFromUrl",0,function(e){if("OrgStackTemplate"===e||"Stack from existing App"===e)return;let t=r(e);if(void 0!==t)return t;try{let t=decodeURIComponent(e);return r(t)}catch{return}},"stackBlueprintDetails",0,a])},496090,e=>{"use strict";var t=e.i(389959);let i="replit:pay-as-you-go-recovery-complete",a=!1;function r(){window.dispatchEvent(new Event(i))}async function n(e){await e(),r()}async function o(e){for(let t=0;t<12;t+=1){let i=await e().catch(()=>null);if(i?.data?.currentUser?.isUBBBanned===!1)return r(),!0;t<11&&await new Promise(e=>setTimeout(e,5e3))}return!1}function s(e,t){o(e).then(e=>{e&&t?.()})}e.s(["notifyPayAsYouGoRecoveryComplete",0,n,"startPayAsYouGoRecoveryPoll",0,s,"startUbbRecoveryPollOnFocus",0,function(e){a||(a=!0,window.addEventListener("focus",()=>{a=!1,s(e)},{once:!0}))},"usePayAsYouGoRecoveryComplete",0,function(e){(0,t.useEffect)(()=>{if(e)return window.addEventListener(i,e),()=>window.removeEventListener(i,e)},[e])}])},966081,e=>{"use strict";var t=e.i(351623),i=e.i(299020);let a={},r=t.gql`
    fragment CustomerSpendingAlertsInitialConfig on CustomerAlerts {
  softAlert {
    id
    threshold
  }
  hardAlert {
    id
    threshold
  }
}
    `,n=t.gql`
    mutation EditCustomerSpendingAlerts($input: UpdateCustomerSpendingAlertsInput!) {
  updateCustomerSpendingAlerts(input: $input) {
    ... on Customer {
      id
      name
    }
    ... on Error {
      message
    }
  }
}
    `;e.s(["CustomerSpendingAlertsInitialConfigFragmentDoc",0,r,"useEditCustomerSpendingAlertsMutation",0,function(e){let t={...a,...e};return i.useMutation(n,t)}])},991349,e=>{"use strict";var t=e.i(351623),i=e.i(344480);e.i(975473);let a={},r=t.gql`
    query IsUBBBanned($orgId: String) {
  currentUser {
    id
    isUBBBanned
    org(orgId: $orgId) {
      ... on Org {
        id
        isUBBBanned
      }
    }
  }
}
    `;e.s(["useIsUbbBannedQuery",0,function(e){let t={...a,...e};return i.useQuery(r,t)}])},815598,e=>{"use strict";var t=e.i(991349);e.s(["useIsUBBBanned",0,function({orgId:e,skip:i=!1}){let{data:a,loading:r,refetch:n}=(0,t.useIsUbbBannedQuery)({variables:{orgId:e},skip:i});return{loading:!i&&r,isUBBBanned:i||a?.currentUser?.org.__typename!=="Org"?!i&&a?.currentUser?.isUBBBanned:a?.currentUser?.org?.isUBBBanned,refetchBanStatus:n}}])},269941,e=>{"use strict";var t=e.i(351623),i=e.i(966081),a=e.i(846545),r=e.i(344480);e.i(975473);var n=e.i(299020);let o={},s=t.gql`
    fragment UsageBasedBillingCreditBalanceDepletedNotification on UsageBasedBillingCreditBalanceDepletedNotification {
  id
  billingPeriodEnd
  isPendingPayAsYouGoAcknowledgement
  isPayAsYouGoPaymentDelinquent
  customer {
    id
    name
    authorizations {
      recoverCreditDepletionPayAsYouGo {
        isAuthorized
      }
      useCreditDepletionPayAsYouGo {
        isAuthorized
      }
      purchaseCreditPack {
        isAuthorized
      }
      isTopUpCustomer
      startAutoTopUp {
        isAuthorized
      }
    }
    subscriptionSummary {
      ... on CustomerSubscriptionSummarySelfServe {
        plan {
          ... on CustomerSubscriptionSummaryTieredSelfServePlan {
            tier
          }
        }
      }
    }
    billing {
      ... on CustomerBilling {
        arrangement {
          ... on CustomerSalesContract {
            id
            isTrial
            canSelfServeEnterpriseBilling
          }
        }
      }
    }
    topUpState {
      ... on CustomerTopUpState {
        isTopUpCustomer
        topUpConfig {
          triggerThresholdUsd
          rechargeAmountUsd
          monthlyCapCents
        }
      }
    }
    orgs {
      ... on OrgConnection {
        items {
          id
          slug
        }
      }
    }
    usageInterval {
      spendingControls {
        ... on CustomerSpendingControls {
          alerts {
            ...CustomerSpendingAlertsInitialConfig
          }
        }
      }
    }
  }
}
    ${i.CustomerSpendingAlertsInitialConfigFragmentDoc}`,l=t.gql`
    subscription UBBCreditDepletedNotifications {
  usageBasedBillingCreditBalanceDepletedNotifications {
    id
    ...UsageBasedBillingCreditBalanceDepletedNotification
  }
}
    ${s}`,u=t.gql`
    query UsageBasedBillingCreditBalanceDepletedCurrentUser {
  currentUser {
    ... on CurrentUser {
      id
      customer {
        id
      }
    }
  }
}
    `,d=t.gql`
    mutation DismissUBBCreditBalanceDepletedNotification($input: UpdateUbbCreditBalanceDepletedNotificationInput!) {
  updateUbbCreditBalanceDepletedNotification(input: $input) {
    ... on UsageBasedBillingCreditBalanceDepletedNotification {
      id
      isDismissed
      isPendingPayAsYouGoAcknowledgement
      isPayAsYouGoPaymentDelinquent
    }
    ... on Error {
      message
    }
  }
}
    `;e.s(["useDismissUbbCreditBalanceDepletedNotificationMutation",0,function(e){let t={...o,...e};return n.useMutation(d,t)},"useUbbCreditDepletedNotificationsSubscription",0,function(e){let t={...o,...e};return a.useSubscription(l,t)},"useUsageBasedBillingCreditBalanceDepletedCurrentUserQuery",0,function(e){let t={...o,...e};return r.useQuery(u,t)}])},662898,e=>{"use strict";var t=e.i(15801),i=e.i(389959),a=e.i(269941),r=e.i(476601),n=e.i(473833),o=e.i(632350),s=e.i(753451);let l={failed:!1,loading:!0,notifications:[],pathname:null,personalCustomerId:void 0,reopen:()=>{}},u=l,d=new Set;function p(e){for(let t of(u=e,d))t()}function c(e){return d.add(e),()=>d.delete(e)}function g({context:e,skip:a=!1,includeBonsaiIdentity:d}){let p=(0,t.useRouter)(),m=(0,s.useIsInBonsaiWebview)(),A=(0,o.default)(),f=(0,n.useDesktopAppHostsRealHome)(),h=(0,i.useSyncExternalStore)(c,()=>u,()=>l),S=(0,r.shouldShowUsageAlert)(p.pathname)&&!m&&(!A||f),y=S||d&&((0,r.shouldShowUsageAlert)(p.pathname)||m),I=h.pathname===p.pathname,_=!a&&y&&I?h.notifications.find(t=>!!t.isPendingPayAsYouGoAcknowledgement&&!t.isPayAsYouGoPaymentDelinquent&&("user"===e.type?t.customer?.id===h.personalCustomerId:t.customer?.orgs.__typename==="OrgConnection"&&t.customer.orgs.items.some(t=>t.id===e.orgId)))??null:null,C=(0,i.useCallback)(()=>{_&&h.reopen(_.id)},[_,h]);return{failed:!a&&y&&I&&h.failed,notification:_,reopen:_&&S?C:null,loading:!a&&y&&(!I||h.loading)}}function m(){let[e]=(0,a.useDismissUbbCreditBalanceDepletedNotificationMutation)(),t=(0,i.useRef)(new Map),r=async(i,a)=>{let r=`${i}:${a}`,n=t.current.get(r);if(n)return n;let o=(async()=>{let t=await e({variables:{input:{notificationId:i,action:a}},refetchQueries:"DISMISS"===a?void 0:["IsUBBBanned"]}),r=t.data?.updateUbbCreditBalanceDepletedNotification;if(r?.__typename!=="UsageBasedBillingCreditBalanceDepletedNotification")throw Error(r?.message??"Failed to update notification")})();t.current.set(r,o);let s=()=>{t.current.get(r)===o&&t.current.delete(r)};return o.then(s,s),o};return{onDismiss:e=>r(e,"DISMISS"),onSuspend:e=>r(e,"SUSPEND"),onAcknowledge:e=>r(e,"ACKNOWLEDGE")}}e.s(["default",0,function({skip:e=!1}={}){let{data:t,loading:r,error:n}=(0,a.useUsageBasedBillingCreditBalanceDepletedCurrentUserQuery)({skip:e,fetchPolicy:"cache-and-network",nextFetchPolicy:"cache-first",errorPolicy:"all",ssr:!1}),o=t?.currentUser?.__typename==="CurrentUser"?t.currentUser:void 0,{data:s,loading:l,error:u}=(0,a.useUbbCreditDepletedNotificationsSubscription)({skip:e||null==o}),[d,p]=(0,i.useState)(new Set),c=e=>{p(t=>new Set([...t,e]))},g=m(),A=(s?.usageBasedBillingCreditBalanceDepletedNotifications??[]).filter(({id:e})=>!d.has(e)),f=o?.customer.id;return{notifications:A,loading:r&&null==o||l&&0===A.length,error:n??u,personalCustomerId:f,onDismiss:e=>(c(e),g.onDismiss(e)),onSuspend:g.onSuspend,onAcknowledge:async e=>{await g.onAcknowledge(e),c(e)},onHide:c}},"resetUbbCreditBalanceDepletedNotificationSource",0,function(){p(l)},"setUbbCreditBalanceDepletedNotificationSource",0,p,"usePendingPayAsYouGoNotification",0,function(e){return g({...e,includeBonsaiIdentity:!1})},"usePendingPayAsYouGoState",0,function(e){return g({...e,includeBonsaiIdentity:!0})},"useUbbCreditBalanceDepletedNotificationActions",0,m])},473833,e=>{"use strict";var t=e.i(526687),i=e.i(68701);e.s(["default",0,function(){let e=(0,i.useUserAgent)();return(0,t.desktopAppUserAgentHostsAtLeast)(e,t.DESKTOP_TAB_BAR_MIN_VERSION)},"useDesktopAppHostsRealHome",0,function(){let e=(0,i.useUserAgent)();return(0,t.desktopAppUserAgentHostsAtLeast)(e,t.DESKTOP_REAL_HOME_MIN_VERSION)}])},890639,e=>{"use strict";var t=e.i(351623),i=e.i(344480);e.i(975473);let a={},r=t.gql`
    fragment AgentWorkspaceAuthorizations on OrgAuthorizations {
  paidAgent: useAiAgent(tier: paid, replId: $replId) {
    __typename
    isAuthorized
    message
    code
  }
  freeAgent: useAiAgent(tier: free) {
    __typename
    isAuthorized
    message
    code
  }
  turboAgentModel: useTurbo(replId: $replId) {
    __typename
    isAuthorized
    message
    code
  }
  quickEditAgentModel: useQuickEdit(replId: $replId) {
    __typename
    isAuthorized
    message
    code
  }
  highEffortAgentModel: useHighEffort {
    __typename
    isAuthorized
    message
    code
  }
  perTierAutoMode: useAgentConfigPerTierAutoMode {
    __typename
    isAuthorized
    message
    code
  }
  intelligentAutoMode: useAgentConfigIntelligentAutoMode {
    __typename
    isAuthorized
    message
    code
  }
  defaultAdvancedAgentModel: defaultAdvancedAgentModel(replId: $replId) {
    __typename
    isAuthorized
    message
    code
  }
  freeModeTier: useAgentConfigFreeModeTier {
    __typename
    isAuthorized
    message
    code
  }
  editSettings {
    __typename
    isAuthorized
    message
    code
  }
}
    `,n=t.gql`
    query GetAgentRepl($replId: String!) {
  getRepl(id: $replId) {
    __typename
    ... on Repl {
      id
      authorizations {
        configureAgentModelSettings {
          isAuthorized
          message
          code
        }
        useFreeLiteTaskAutoApproval {
          isAuthorized
          message
          code
        }
      }
      org {
        id
        slug
        dealContext {
          dealType
        }
      }
      user {
        id
      }
    }
    ... on Error {
      message
    }
  }
}
    `,o=t.gql`
    query GetAgentAutoPublishAuthorization($replId: String!, $isPrivate: Boolean!) {
  getRepl(id: $replId) {
    __typename
    ... on Repl {
      id
      authorizations {
        autoPublishDeployment: redeployDeployment(
          provider: cloud_run
          isPrivate: $isPrivate
        ) {
          isAuthorized
        }
      }
    }
    ... on Error {
      message
    }
  }
}
    `,s=t.gql`
    query GetAgentReplAuthorizations($orgId: String, $ownerUserId: Int!, $replId: String!) {
  currentUser {
    id
    personalOrgAuthorizations {
      ... on OrgAuthorizations {
        ...AgentWorkspaceAuthorizations
      }
      ... on Error {
        message
      }
    }
    org(orgId: $orgId) {
      ... on Org {
        id
        authorizations {
          ... on OrgAuthorizations {
            ...AgentWorkspaceAuthorizations
          }
        }
      }
      ... on Error {
        message
      }
    }
  }
  replOwner: user(id: $ownerUserId) {
    id
    paidAgentAuthorization(replId: $replId) {
      ... on OrgAuthorization {
        isAuthorized
        message
        code
      }
      ... on Error {
        message
      }
    }
    defaultAdvancedAgentModelAuthorization(replId: $replId) {
      ... on OrgAuthorization {
        isAuthorized
        message
        code
      }
      ... on Error {
        message
      }
    }
    turboAgentModelAuthorization(replId: $replId) {
      ... on OrgAuthorization {
        isAuthorized
        message
        code
      }
      ... on Error {
        message
      }
    }
  }
}
    ${r}`;e.s(["useGetAgentAutoPublishAuthorizationQuery",0,function(e){let t={...a,...e};return i.useQuery(o,t)},"useGetAgentReplAuthorizationsQuery",0,function(e){let t={...a,...e};return i.useQuery(s,t)},"useGetAgentReplQuery",0,function(e){let t={...a,...e};return i.useQuery(n,t)}])},29079,e=>{"use strict";var t=e.i(908796),i=e.i(890639),a=e.i(211719);let r={__typename:"OrgAuthorization",isAuthorized:!1,message:"Free agent is not available in personal repls for guests",code:t.OrgAuthorizationCode.InsufficientPermissions},n={__typename:"OrgAuthorization",isAuthorized:!0,message:"",code:t.OrgAuthorizationCode.Authorized},o={__typename:"OrgAuthorization",isAuthorized:!1,message:"Quick Edit authorization is not available for guests",code:t.OrgAuthorizationCode.InsufficientPermissions},s={__typename:"OrgAuthorization",isAuthorized:!1,message:"Auto model selection is not available for guests",code:t.OrgAuthorizationCode.InsufficientPermissions},l={__typename:"OrgAuthorization",isAuthorized:!1,message:"Auto mode is not available for guests",code:t.OrgAuthorizationCode.InsufficientPermissions},u={__typename:"OrgAuthorization",isAuthorized:!1,message:"Workspace settings are not editable by guests",code:t.OrgAuthorizationCode.InsufficientPermissions};e.s(["useAgentAuthorization",0,function(e){let d=(0,a.default)(),p=e?.replId??d,{orgId:c,orgSlug:g,orgDealType:m,ownerUserId:A,configureAgentModelSettings:f,freeLiteTaskAutoApproval:h,refetch:S,loading:y,error:I}=function(e,t=!1){let r=(0,a.default)(),n=e??r,{data:o,loading:s,error:l,refetch:u}=(0,i.useGetAgentReplQuery)({variables:{replId:n??""},skip:t||null===n});return{orgId:o?.getRepl.__typename==="Repl"?o.getRepl.org?.id:void 0,orgSlug:o?.getRepl.__typename==="Repl"?o.getRepl.org?.slug:void 0,orgDealType:o?.getRepl.__typename==="Repl"?o.getRepl.org?.dealContext.dealType:void 0,ownerUserId:o?.getRepl.__typename==="Repl"?o.getRepl.user?.id:void 0,configureAgentModelSettings:o?.getRepl.__typename==="Repl"?o.getRepl.authorizations?.configureAgentModelSettings:void 0,freeLiteTaskAutoApproval:o?.getRepl.__typename==="Repl"?o.getRepl.authorizations?.useFreeLiteTaskAutoApproval:void 0,loading:s,error:l,refetch:u}}(e?.replId,e?.skip),{data:_,loading:C,error:B,refetch:k}=(0,i.useGetAgentReplAuthorizationsQuery)({variables:{orgId:c,ownerUserId:A??0,replId:p??""},skip:e?.skip||!A||null===p,pollInterval:e?.pollInterval&&c?e.pollInterval:void 0}),P=()=>{e?.skip||null===p||(S(),k())},N=_?.currentUser?.id,U=!!N&&!!A&&N===A,T={configureAgentModelSettings:f,freeLiteTaskAutoApproval:h};if(y||C)return{loading:!0,refetch:P,orgId:void 0,orgSlug:void 0,...T};if(I||B)return{loading:!1,refetch:P,orgId:void 0,orgSlug:void 0,...T};if(c)return{authorizations:_?.currentUser?.org.__typename==="Org"?_.currentUser.org.authorizations:void 0,loading:!1,refetch:P,isOwner:U,isOrgRepl:!0,isGuestInPersonalRepl:!1,orgId:c,orgSlug:g,orgDealType:m,...T};if(!A)return{authorizations:void 0,loading:!1,refetch:P,isOwner:U,isOrgRepl:!1,orgSlug:void 0,...T};if(U){let e=_?.currentUser?.personalOrgAuthorizations;return{authorizations:e?.__typename==="OrgAuthorizations"?e:void 0,loading:!1,refetch:P,isOwner:U,isOrgRepl:!1,isGuestInPersonalRepl:!1,orgSlug:void 0,...T}}let R=_?.replOwner?.paidAgentAuthorization,v=_?.replOwner?.defaultAdvancedAgentModelAuthorization,z=_?.replOwner?.turboAgentModelAuthorization,b=_?.currentUser?.personalOrgAuthorizations,E=b?.__typename==="OrgAuthorizations"?b.quickEditAgentModel:void 0,M=b?.__typename==="OrgAuthorizations"?b.perTierAutoMode:void 0;return R?.__typename==="OrgAuthorization"?{authorizations:{__typename:"OrgAuthorizations",paidAgent:R,freeAgent:r,turboAgentModel:z?.__typename==="OrgAuthorization"?z:{__typename:"OrgAuthorization",isAuthorized:!1,message:"Turbo mode authorization is not available for guests",code:t.OrgAuthorizationCode.InsufficientPermissions},quickEditAgentModel:E??o,highEffortAgentModel:{__typename:"OrgAuthorization",isAuthorized:!0,message:"",code:t.OrgAuthorizationCode.Authorized},perTierAutoMode:M??s,intelligentAutoMode:l,defaultAdvancedAgentModel:v?.__typename==="OrgAuthorization"?v:{__typename:"OrgAuthorization",isAuthorized:!1,message:"Default advanced agent model check is not available for guests",code:t.OrgAuthorizationCode.InsufficientPermissions},freeModeTier:n,editSettings:u},loading:!1,refetch:P,isOwner:U,isOrgRepl:!1,isGuestInPersonalRepl:!0,orgSlug:void 0,...T}:{authorizations:void 0,loading:!1,refetch:P,isOwner:U,isOrgRepl:!1,isGuestInPersonalRepl:!0,orgSlug:void 0,configureAgentModelSettings:f,freeLiteTaskAutoApproval:h}},"useAgentAutoPublishAuthorization",0,function({replId:e,isPrivate:t,skip:a=!1}){let{data:r,error:n,loading:o}=(0,i.useGetAgentAutoPublishAuthorizationQuery)({variables:{replId:e,isPrivate:t},skip:a,fetchPolicy:"cache-first"}),s=r?.getRepl.__typename==="Repl"?r.getRepl.authorizations.autoPublishDeployment:void 0;return{isAuthorized:void 0===n&&s?.isAuthorized===!0,loading:o}}])},943427,e=>{"use strict";let t={web:!0,mobile:!0,slides:!0,video:!0,game:!0,"data-app":!0,automation:!0,"design-system":!0,cli:!1,vnc:!1,api:!1,design:!1,custom:!1,legacy:!1};function i(e){return void 0===e||(t[e]??!0)}e.s(["isArtifactKindPreviewable",0,i,"isArtifactPreviewable",0,function(e){return void 0!==e.isPreviewable?e.isPreviewable:i(e.kind)}])},571642,e=>{"use strict";var t=e.i(661832);e.s(["thumbhashToDataUrl",0,function(e){return(0,t.thumbHashToDataURL)(function(e){let t=atob(e),i=new Uint8Array(t.length);for(let e=0;e<t.length;e++)i[e]=t.charCodeAt(e);return i}(e))}])},399245,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 1.25c5.937 0 10.75 4.813 10.75 10.75S17.937 22.75 12 22.75 1.25 17.937 1.25 12 6.063 1.25 12 1.25m-9.217 11.5a9.25 9.25 0 0 0 7.468 8.333 15.25 15.25 0 0 1-2.982-8.333zm13.948 0a15.25 15.25 0 0 1-2.983 8.333 9.25 9.25 0 0 0 7.469-8.333zm-7.958 0A13.75 13.75 0 0 0 12 20.876a13.75 13.75 0 0 0 3.227-8.126zm1.478-9.834a9.25 9.25 0 0 0-7.468 8.334H7.27a15.25 15.25 0 0 1 2.982-8.334M12 3.123a13.75 13.75 0 0 0-3.227 8.127h6.454A13.75 13.75 0 0 0 12 3.123m1.748-.207a15.25 15.25 0 0 1 2.983 8.334h4.486a9.25 9.25 0 0 0-7.469-8.334",clipRule:"evenodd"})})}])},927225,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M3.25 12a1.75 1.75 0 1 1 3.5 0 1.75 1.75 0 0 1-3.5 0m7 0a1.75 1.75 0 1 1 3.5 0 1.75 1.75 0 0 1-3.5 0m7 0a1.75 1.75 0 1 1 3.5 0 1.75 1.75 0 0 1-3.5 0",clipRule:"evenodd"})})}])},735362,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M6.01 1.25a.75.75 0 0 1 .7.48l.716 1.863 1.862.716a.75.75 0 0 1 0 1.4l-1.862.717-.717 1.862a.75.75 0 0 1-1.4 0l-.716-1.862-1.862-.717a.75.75 0 0 1 0-1.4l1.862-.716.716-1.862a.75.75 0 0 1 .7-.481m0 2.84-.137.353a.75.75 0 0 1-.43.43l-.354.136.354.136a.75.75 0 0 1 .43.431l.136.353.136-.353a.75.75 0 0 1 .431-.43l.353-.137-.353-.136a.75.75 0 0 1-.43-.43zm12.04-1.503a.75.75 0 0 1 .7.481l.995 2.587 2.587.995a.75.75 0 0 1 0 1.4l-2.587.995-.995 2.587a.75.75 0 0 1-1.4 0l-.995-2.587-2.587-.995a.75.75 0 0 1 0-1.4l2.587-.995.995-2.587a.75.75 0 0 1 .7-.48m0 2.84-.415 1.077a.75.75 0 0 1-.43.431l-1.078.415 1.078.415a.75.75 0 0 1 .43.43l.415 1.078.415-1.078a.75.75 0 0 1 .43-.43l1.079-.415-1.079-.415a.75.75 0 0 1-.43-.43zm-7.662 1.37a1.126 1.126 0 0 1 1.688.525l1.76 4.574 4.574 1.76a1.126 1.126 0 0 1 0 2.101l-4.574 1.76-1.76 4.574a1.126 1.126 0 0 1-2.102 0l-1.76-4.574-4.573-1.76a1.126 1.126 0 0 1 0-2.102l4.574-1.76 1.76-4.573c.08-.212.225-.395.413-.524m.637 1.97-1.47 3.822a1.13 1.13 0 0 1-.647.647l-3.822 1.47 3.822 1.47a1.13 1.13 0 0 1 .647.647l1.47 3.822 1.47-3.822a1.13 1.13 0 0 1 .647-.647l3.822-1.47-3.822-1.47a1.13 1.13 0 0 1-.647-.647zm-.349 12.785",clipRule:"evenodd"})})}])},813707,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M14.017 1.543a6.75 6.75 0 0 1 3.77-.056l.192.068c.91.39.97 1.556.36 2.17l-3.104 3.1a.25.25 0 0 0 0 .35l1.594 1.593a.25.25 0 0 0 .17.068.25.25 0 0 0 .171-.068l3.106-3.104c.654-.655 1.937-.538 2.236.55l.06.232a6.75 6.75 0 0 1-8.863 7.905l-7.56 7.56A2.872 2.872 0 0 1 2.09 17.85l7.559-7.56a6.75 6.75 0 0 1 4.368-8.747M17.08 2.86a5.25 5.25 0 0 0-2.622.117 5.25 5.25 0 0 0-3.244 7.183.75.75 0 0 1-.153.84l-7.91 7.91a1.372 1.372 0 0 0 1.938 1.94L13 12.94a.75.75 0 0 1 .84-.154 5.25 5.25 0 0 0 7.3-5.864l-2.914 2.913a1.75 1.75 0 0 1-2.45 0l-1.606-1.604-.004-.006a1.75 1.75 0 0 1 0-2.45z",clipRule:"evenodd"})})}])},968323,e=>{"use strict";function t(e,t){return{ok:!1,error:e,errorExtras:t}}function i(e){return{ok:!0,value:e}}let a=async(e,a)=>{try{let t=await e();return i(t)}catch(e){return t(a?a(e):e)}};e.s(["Err",0,t,"Ok",0,i,"default",0,{Ok:i,Err:t,tryCatch:(e,a)=>{try{return i(e())}catch(e){return t(a?a(e):e)}},tryCatchAsync:a},"tryCatchAsync",0,a])}]);

//# debugId=07a4aa8d-f1dc-c763-3c0c-558c7b304f67
//# sourceMappingURL=1m179_h5r2ae1.js.map