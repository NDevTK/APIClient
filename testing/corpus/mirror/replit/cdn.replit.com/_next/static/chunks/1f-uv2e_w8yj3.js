;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="a6ab4c20-aba2-d7c6-fbbe-44bfa33c1e59")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,411719,(e,t,o)=>{"use strict";Object.defineProperty(o,"__esModule",{value:!0}),Object.defineProperty(o,"LoadableContext",{enumerable:!0,get:function(){return r}});let r=e.r(2879)._(e.r(389959)).default.createContext(null)},46147,(e,t,o)=>{"use strict";Object.defineProperty(o,"__esModule",{value:!0}),Object.defineProperty(o,"default",{enumerable:!0,get:function(){return g}});let r=e.r(2879)._(e.r(389959)),n=e.r(411719),i=[],l=[],s=!1;function a(e){let t=e(),o={loading:!0,loaded:null,error:null};return o.promise=t.then(e=>(o.loading=!1,o.loaded=e,e)).catch(e=>{throw o.loading=!1,o.error=e,e}),o}class u{constructor(e,t){this._loadFn=e,this._opts=t,this._callbacks=new Set,this._delay=null,this._timeout=null,this.retry()}promise(){return this._res.promise}retry(){this._clearTimeouts(),this._res=this._loadFn(this._opts.loader),this._state={pastDelay:!1,timedOut:!1};let{_res:e,_opts:t}=this;e.loading&&("number"==typeof t.delay&&(0===t.delay?this._state.pastDelay=!0:this._delay=setTimeout(()=>{this._update({pastDelay:!0})},t.delay)),"number"==typeof t.timeout&&(this._timeout=setTimeout(()=>{this._update({timedOut:!0})},t.timeout))),this._res.promise.then(()=>{this._update({}),this._clearTimeouts()}).catch(e=>{this._update({}),this._clearTimeouts()}),this._update({})}_update(e){this._state={...this._state,error:this._res.error,loaded:this._res.loaded,loading:this._res.loading,...e},this._callbacks.forEach(e=>e())}_clearTimeouts(){clearTimeout(this._delay),clearTimeout(this._timeout)}getCurrentValue(){return this._state}subscribe(e){return this._callbacks.add(e),()=>{this._callbacks.delete(e)}}}function d(t){return function(t,o){let a=Object.assign({loader:null,loading:null,delay:200,timeout:null,webpack:null,modules:null},o),d=null;function p(){if(!d){let e=new u(t,a);d={getCurrentValue:e.getCurrentValue.bind(e),subscribe:e.subscribe.bind(e),retry:e.retry.bind(e),promise:e.promise.bind(e)}}return d.promise()}if("u"<typeof window&&i.push(p),!s&&"u">typeof window){let t=a.webpack&&"function"==typeof e.t.resolveWeak?a.webpack():a.modules;t&&l.push(e=>{for(let o of t)if(e.includes(o))return p()})}function g(e,t){let o;p(),(o=r.default.useContext(n.LoadableContext))&&Array.isArray(a.modules)&&a.modules.forEach(e=>{o(e)});let i=r.default.useSyncExternalStore(d.subscribe,d.getCurrentValue,d.getCurrentValue);return r.default.useImperativeHandle(t,()=>({retry:d.retry}),[]),r.default.useMemo(()=>{var t;return i.loading||i.error?r.default.createElement(a.loading,{isLoading:i.loading,pastDelay:i.pastDelay,timedOut:i.timedOut,error:i.error,retry:d.retry}):i.loaded?r.default.createElement((t=i.loaded)&&t.default?t.default:t,e):null},[e,i])}return g.preload=()=>p(),g.displayName="LoadableComponent",r.default.forwardRef(g)}(a,t)}function p(e,t){let o=[];for(;e.length;){let r=e.pop();o.push(r(t))}return Promise.all(o).then(()=>{if(e.length)return p(e,t)})}d.preloadAll=()=>new Promise((e,t)=>{p(i).then(e,t)}),d.preloadReady=(e=[])=>new Promise(t=>{let o=()=>(s=!0,t());p(l,e).then(o,o)}),"u">typeof window&&(window.__NEXT_PRELOADREADY=d.preloadReady);let g=d},493092,(e,t,o)=>{"use strict";Object.defineProperty(o,"__esModule",{value:!0});var r={default:function(){return p},noSSR:function(){return d}};for(var n in r)Object.defineProperty(o,n,{enumerable:!0,get:r[n]});let i=e.r(2879),l=e.r(478902);e.r(389959);let s=i._(e.r(46147)),a="u"<typeof window;function u(e){return{default:e?.default||e}}function d(e,t){if(delete t.webpack,delete t.modules,!a)return e(t);let o=t.loading;return()=>(0,l.jsx)(o,{error:null,isLoading:!0,pastDelay:!1,timedOut:!1})}function p(e,t){let o=s.default,r={loading:({error:e,isLoading:t,pastDelay:o})=>null};e instanceof Promise?r.loader=()=>e:"function"==typeof e?r.loader=e:"object"==typeof e&&(r={...r,...e});let n=(r={...r,...t}).loader;return(r.loadableGenerated&&(r={...r,...r.loadableGenerated},delete r.loadableGenerated),"boolean"!=typeof r.ssr||r.ssr)?o({...r,loader:()=>null!=n?n().then(u):Promise.resolve(u(()=>null))}):(delete r.webpack,delete r.modules,d(o,r))}("function"==typeof o.default||"object"==typeof o.default&&null!==o.default)&&void 0===o.default.__esModule&&(Object.defineProperty(o.default,"__esModule",{value:!0}),Object.assign(o.default,o),t.exports=o.default)},196786,(e,t,o)=>{t.exports=e.r(493092)},211025,e=>{"use strict";let t=["low","medium","high","xhigh","max"],o=["low","medium","high"];e.s(["CAPPED_EFFORT_LEVELS",0,o,"EFFORT_LEVELS",0,t,"EFFORT_LEVEL_LABELS",0,{low:{label:"Low",labelId:"workspace.agentSettingsEffortNameLow"},medium:{label:"Medium",labelId:"workspace.agentSettingsEffortNameMedium"},high:{label:"High",labelId:"workspace.agentSettingsEffortNameHigh"},xhigh:{label:"Extra High",labelId:"workspace.agentSettingsEffortNameExtraHigh"},max:{label:"Max",labelId:"workspace.agentSettingsEffortNameMax"}},"clampToLadder",0,function(e,t){return t.includes(e)?e:t[t.length-1]},"effortLevelsFor",0,function(e){return e?o:t}])},621738,e=>{"use strict";e.s(["DEFAULT_CHATEAU_AGENT_CONFIG",0,{mode:"UNSPECIFIED",modelProfile:"UNSPECIFIED",enableTurbo:void 0,enableAutomatedTesting:void 0,enableHighEffort:void 0,autoApprovePlan:void 0,autoMerge:void 0,autoPublish:void 0,intelligentAutoMode:void 0,liteModel:void 0,economyModel:void 0,powerModel:void 0,modelEfforts:void 0},"applyAgentConfigDiff",0,function(e,t){let o={...e,...t};if(!("modelEfforts"in t)||void 0===t.modelEfforts)return o;let r={...e.modelEfforts};for(let[e,o]of Object.entries(t.modelEfforts))void 0===o?delete r[e]:r[e]=o;return o.modelEfforts=Object.keys(r).length>0?r:void 0,o}])},394701,e=>{"use strict";var t=e.i(351623),o=e.i(344480);e.i(975473);var r=e.i(299020);let n={},i=t.gql`
    fragment TransferReplBetweenWorkspacesDialogRepl on Repl {
  id
  title
}
    `,l=t.gql`
    query TransferReplBetweenWorkspacesDialogDestinations($replId: String!) {
  getRepl(id: $replId) {
    __typename
    ... on Repl {
      id
      transferDestinationOrgs {
        __typename
        ... on ReplTransferDestinationOrgsSuccess {
          items {
            id
            name
            slug
            image
          }
        }
        ... on Error {
          message
        }
      }
    }
    ... on Error {
      message
    }
  }
}
    `,s=t.gql`
    mutation TransferReplBetweenWorkspacesDialogTransfer($replId: String!, $destinationOrgId: String!) {
  transferReplBetweenOrganizations(
    input: {replId: $replId, destinationOrgId: $destinationOrgId}
  ) {
    __typename
    ... on TransferReplBetweenOrganizationsSuccess {
      runId
      repl {
        id
      }
    }
    ... on NotFoundError {
      message
    }
    ... on UnauthorizedError {
      message
    }
    ... on UserError {
      message
    }
    ... on TooManyRequestsError {
      message
    }
  }
}
    `;e.s(["TransferReplBetweenWorkspacesDialogReplFragmentDoc",0,i,"useTransferReplBetweenWorkspacesDialogDestinationsQuery",0,function(e){let t={...n,...e};return o.useQuery(l,t)},"useTransferReplBetweenWorkspacesDialogTransferMutation",0,function(e){let t={...n,...e};return r.useMutation(s,t)}])},781258,e=>{"use strict";var t=e.i(351623),o=e.i(344480);e.i(975473);var r=e.i(299020);let n={},i=t.gql`
    fragment TransferReplToOrgDialogRepl on Repl {
  id
  title
  slug
  owner {
    ... on User {
      id
      username
    }
    ... on Team {
      id
      username
    }
  }
}
    `,l=t.gql`
    query TransferReplToOrgDialogOrgs {
  currentUser {
    id
    orgs(count: 30) {
      __typename
      ... on CurrentUserOrganizationConnection {
        items {
          org {
            id
            name
            slug
            image
            type
          }
          type
        }
      }
      ... on Error {
        message
      }
    }
  }
}
    `,s=t.gql`
    mutation TransferReplToOrgDialogTransfer($orgId: String!, $replIds: [String!]!) {
  transferReplToOrganization(input: {orgId: $orgId, replIds: $replIds}) {
    ... on TransferReplToOrganizationSuccess {
      runId
      results {
        replId
        success
        error
      }
      successCount
      errorCount
    }
    ... on UnauthorizedError {
      message
    }
    ... on UserError {
      message
    }
    ... on TooManyRequestsError {
      message
    }
  }
}
    `;e.s(["TransferReplToOrgDialogReplFragmentDoc",0,i,"useTransferReplToOrgDialogOrgsQuery",0,function(e){let t={...n,...e};return o.useQuery(l,t)},"useTransferReplToOrgDialogTransferMutation",0,function(e){let t={...n,...e};return r.useMutation(s,t)}])},618876,e=>{"use strict";var t=e.i(960933),o=e.i(335421),r=e.i(211025),n=e.i(621738),i=e.i(987997),l=e.i(489859),s=e.i(615593);let a={build:"creation-agent-model-picks",design:"creation-agent-design-model-picks"};function u(e,t){return(0,i.creationAgentStorageKey)(a[e],t)}let d=t.Type.Object({liteModel:t.Type.Optional(t.Type.String()),economyModel:t.Type.Optional(t.Type.String()),powerModel:t.Type.Optional(t.Type.String()),modelEfforts:t.Type.Optional(t.Type.Record(t.Type.String(),t.Type.String()))});function p(e){let t={};for(let[n,i]of(void 0!==e.liteModel&&(t=g(t,(0,s.getAgentConfigDiffForModelPick)("lite",e.liteModel))),void 0!==e.economyModel&&(t=g(t,(0,s.getAgentConfigDiffForModelPick)("economy",e.economyModel))),void 0!==e.powerModel&&(t=g(t,(0,s.getAgentConfigDiffForModelPick)("power",e.powerModel))),Object.entries(e.modelEfforts??{}))){var o;o=i,r.EFFORT_LEVELS.includes(o)&&(t=g(t,(0,s.getAgentConfigDiffForEffortPick)(n,i)))}return t}function g(e,t){if(null===t)return e;let o=(0,n.applyAgentConfigDiff)({...n.DEFAULT_CHATEAU_AGENT_CONFIG,...e},t);return{...void 0!==o.liteModel&&{liteModel:o.liteModel},...void 0!==o.economyModel&&{economyModel:o.economyModel},...void 0!==o.powerModel&&{powerModel:o.powerModel},...void 0!==o.modelEfforts&&{modelEfforts:o.modelEfforts}}}e.s(["applyCreationAgentModelPicks",0,g,"capCreationAgentModelPicks",0,function(e,t){return t&&void 0!==e.modelEfforts?{...e,modelEfforts:Object.fromEntries(Object.entries(e.modelEfforts).map(([e,t])=>[e,(0,r.clampToLadder)(t,r.CAPPED_EFFORT_LEVELS)]))}:e},"creationAgentModelPicksFromStored",0,p,"filterCreationAgentModelPicks",0,function(e,t,r=!0){let n=Object.values(t).flatMap(e=>e??[]),i=Object.fromEntries(Object.entries(e.modelEfforts??{}).filter(([e])=>n.some(t=>t.slug===e&&null!==t.recommendedEffort&&void 0===t.authorization))),l=e=>r||!o.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e);return{...void 0!==e.liteModel&&l(e.liteModel)&&t.lite?.some(t=>t.slug===e.liteModel&&void 0===t.authorization)&&{liteModel:e.liteModel},...void 0!==e.economyModel&&l(e.economyModel)&&t.economy?.some(t=>t.slug===e.economyModel&&void 0===t.authorization)&&{economyModel:e.economyModel},...void 0!==e.powerModel&&l(e.powerModel)&&t.power?.some(t=>t.slug===e.powerModel&&void 0===t.authorization)&&{powerModel:e.powerModel},...Object.keys(i).length>0&&{modelEfforts:i}}},"readCreationAgentModelPicks",0,function(e,t){let o=l.default.get(u(e,t),d);return null===o?{}:p(o)},"writeCreationAgentModelPicks",0,function(e,t,o){l.default.set(u(t,o),e)}])},987997,e=>{"use strict";e.s(["creationAgentStorageKey",0,function(e,t){return void 0===t?e:`${e}:org:${t}`}])},328789,e=>{"use strict";var t=e.i(908796),o=e.i(11990),r=e.i(462669);let n=[300,300,300,300];async function i({replId:e,modelProfile:o,isPlanModeEnabled:r,isModelProfileChosen:l=!0,persist:s,onError:a,retryDelaysMs:u=n}){let d=function(e,o=!0){if(o)switch(e){case"FREE":return t.UserReplModelTier.Free;case"LITE":return t.UserReplModelTier.Lite;case"ECONOMY":return t.UserReplModelTier.Economy;case"POWER":case"TURBO":return t.UserReplModelTier.Power;case"UNSPECIFIED":case void 0:return}}(o,l);if(l&&void 0===d)return!1;for(let t=0;;t+=1){try{let t=await s(e,d,r);if("persisted"===t)return!0}catch(e){return a(e instanceof Error?e:Error(String(e))),!1}let o=u[t];if(void 0===o)return a(Error("Could not find the new repl to persist Agent mode")),!1;await new Promise(e=>{setTimeout(e,o)})}}e.s(["PERSIST_AGENT_MODE_SETTINGS_TIMEOUT_MS",0,5e3,"getCreationModePickerAgentSettings",0,function({modelTier:e,isPlanModeEnabled:t,modelPicks:n,modelSettingsMode:i,intelligentAutoMode:l}){let{designModeSettings:s,...a}=(0,r.mapChateauConfigDiffToUserSettingsInput)(n,i).agentSettings,u=void 0===e?{}:{modelTier:e},d=!0===l&&n.powerModel===o.INTELLIGENT_AUTO_MODEL_SLUG?(0,r.mapChateauConfigDiffToUserSettingsInput)({powerModel:n.powerModel},"build").agentSettings:{},p=void 0===l?{}:(0,r.mapChateauConfigDiffToUserSettingsInput)({intelligentAutoMode:l},"build").agentSettings,g={...u,...d,...s??{}};return{...u,isPlanModeEnabled:t,...d,...p,...a,...Object.keys(g).length>0&&{designModeSettings:g}}},"persistCreationModePickerSettings",0,i])},317349,e=>{"use strict";var t=e.i(351623),o=e.i(299020);let r={},n=t.gql`
    fragment DeleteReplDialogRepl on Repl {
  id
  title
}
    `,i=t.gql`
    mutation DeleteReplDialogReplDelete($id: String!) {
  deleteRepl(id: $id) {
    id
  }
}
    `;e.s(["DeleteReplDialogReplFragmentDoc",0,n,"useDeleteReplDialogReplDeleteMutation",0,function(e){let t={...r,...e};return o.useMutation(i,t)}])},748538,e=>{"use strict";var t=e.i(351623),o=e.i(299020);let r={},n=t.gql`
    fragment EditReplFormRepl on Repl {
  id
  title
  description
  imageUrl
  iconUrl
  templateInfo {
    iconUrl
    imageUrl
  }
  authorizations {
    editMetadata {
      isAuthorized
      code
      message
    }
  }
}
    `,i=t.gql`
    mutation EditReplFormEdit($input: UpdateReplInput!) {
  updateRepl(input: $input) {
    repl {
      id
      ...EditReplFormRepl
    }
  }
}
    ${n}`;e.s(["EditReplFormReplFragmentDoc",0,n,"useEditReplFormEditMutation",0,function(e){let t={...r,...e};return o.useMutation(i,t)}])},80593,e=>{"use strict";var t=e.i(351623),o=e.i(299020);let r={},n=t.gql`
    fragment LeaveMultiplayerReplDialogRepl on Repl {
  id
  title
}
    `,i=t.gql`
    mutation LeaveMultiplayerReplDialogRemove($id: String!) {
  removeSharedRepl(replId: $id) {
    id
  }
}
    `;e.s(["LeaveMultiplayerReplDialogReplFragmentDoc",0,n,"useLeaveMultiplayerReplDialogRemoveMutation",0,function(e){let t={...r,...e};return o.useMutation(i,t)}])},349565,e=>{"use strict";e.s(["ECONOMY_MODEL_BY_GQL",0,{CLAUDE_4_6_SONNET:"claude-4-6-sonnet",GPT_5_6_LUNA:"gpt-5-6-luna",GPT_5_6_LUNA_FAST:"gpt-5-6-luna-fast",CLAUDE_5_SONNET:"claude-5-sonnet",GPT_5_6_TERRA:"gpt-5-6-terra",GEMINI_3_1_PRO:"gemini-3-1-pro",GLM_5_2:"glm-5-2",ECONOMY_AUTO:"economy-auto",DESIGN_ECONOMY_AUTO:"design-economy-auto"},"ECONOMY_MODEL_SLUGS",0,["claude-4-6-sonnet","gpt-5-6-luna","gpt-5-6-luna-fast","claude-5-sonnet","gpt-5-6-terra","gemini-3-1-pro","glm-5-2","economy-auto","design-economy-auto"],"GQL_BY_MODEL_SLUG",0,{"free-auto":"FREE_AUTO","kimi-k2-7":"KIMI_K2_7","gpt-5-6-luna":"GPT_5_6_LUNA","gemini-3-5-flash":"GEMINI_3_5_FLASH","deepseek-v4-flash":"DEEPSEEK_V4_FLASH","claude-4-6-sonnet":"CLAUDE_4_6_SONNET","gpt-5-6-luna-fast":"GPT_5_6_LUNA_FAST","claude-5-sonnet":"CLAUDE_5_SONNET","gpt-5-6-terra":"GPT_5_6_TERRA","gemini-3-1-pro":"GEMINI_3_1_PRO","glm-5-2":"GLM_5_2","economy-auto":"ECONOMY_AUTO","design-economy-auto":"DESIGN_ECONOMY_AUTO","claude-fable-5":"CLAUDE_FABLE_5","claude-fable-5-1":"CLAUDE_FABLE_5_1","power-auto":"POWER_AUTO","intelligent-auto":"INTELLIGENT_AUTO","design-power-auto":"DESIGN_POWER_AUTO","claude-4-8-opus":"CLAUDE_4_8_OPUS","claude-4-8-opus-fast":"CLAUDE_4_8_OPUS_FAST","claude-5-opus":"CLAUDE_5_OPUS","claude-5-opus-fast":"CLAUDE_5_OPUS_FAST","kimi-k3":"KIMI_K3","gpt-5-6-terra-fast":"GPT_5_6_TERRA_FAST","gpt-5-6-sol":"GPT_5_6_SOL","gpt-6-astra":"GPT_6_ASTRA"},"LITE_MODEL_BY_GQL",0,{KIMI_K2_7:"kimi-k2-7",GPT_5_6_LUNA:"gpt-5-6-luna",GEMINI_3_5_FLASH:"gemini-3-5-flash",DEEPSEEK_V4_FLASH:"deepseek-v4-flash"},"LITE_MODEL_SLUGS",0,["kimi-k2-7","gpt-5-6-luna","gemini-3-5-flash","deepseek-v4-flash"],"MODEL_SLUG_BY_GQL",0,{FREE_AUTO:"free-auto",KIMI_K2_7:"kimi-k2-7",GPT_5_6_LUNA:"gpt-5-6-luna",GEMINI_3_5_FLASH:"gemini-3-5-flash",DEEPSEEK_V4_FLASH:"deepseek-v4-flash",CLAUDE_4_6_SONNET:"claude-4-6-sonnet",GPT_5_6_LUNA_FAST:"gpt-5-6-luna-fast",CLAUDE_5_SONNET:"claude-5-sonnet",GPT_5_6_TERRA:"gpt-5-6-terra",GEMINI_3_1_PRO:"gemini-3-1-pro",GLM_5_2:"glm-5-2",ECONOMY_AUTO:"economy-auto",DESIGN_ECONOMY_AUTO:"design-economy-auto",CLAUDE_FABLE_5:"claude-fable-5",CLAUDE_FABLE_5_1:"claude-fable-5-1",POWER_AUTO:"power-auto",INTELLIGENT_AUTO:"intelligent-auto",DESIGN_POWER_AUTO:"design-power-auto",CLAUDE_4_8_OPUS:"claude-4-8-opus",CLAUDE_4_8_OPUS_FAST:"claude-4-8-opus-fast",CLAUDE_5_OPUS:"claude-5-opus",CLAUDE_5_OPUS_FAST:"claude-5-opus-fast",KIMI_K3:"kimi-k3",GPT_5_6_TERRA_FAST:"gpt-5-6-terra-fast",GPT_5_6_SOL:"gpt-5-6-sol",GPT_6_ASTRA:"gpt-6-astra"},"POWER_MODEL_BY_GQL",0,{CLAUDE_FABLE_5:"claude-fable-5",CLAUDE_FABLE_5_1:"claude-fable-5-1",POWER_AUTO:"power-auto",INTELLIGENT_AUTO:"intelligent-auto",DESIGN_POWER_AUTO:"design-power-auto",CLAUDE_4_8_OPUS:"claude-4-8-opus",CLAUDE_4_8_OPUS_FAST:"claude-4-8-opus-fast",CLAUDE_5_OPUS:"claude-5-opus",CLAUDE_5_OPUS_FAST:"claude-5-opus-fast",KIMI_K3:"kimi-k3",GPT_5_6_TERRA_FAST:"gpt-5-6-terra-fast",GPT_5_6_SOL:"gpt-5-6-sol",GPT_6_ASTRA:"gpt-6-astra"},"POWER_MODEL_SLUGS",0,["claude-fable-5","claude-fable-5-1","power-auto","intelligent-auto","design-power-auto","claude-4-8-opus","claude-4-8-opus-fast","claude-5-opus","claude-5-opus-fast","kimi-k3","gpt-5-6-terra-fast","gpt-5-6-sol","gpt-6-astra"]])},615593,e=>{"use strict";var t=e.i(349565);function o(e){switch(e.modelProfile){case"FREE":return"free";case"LITE":return"lite";case"POWER":case"TURBO":return"power";case"ECONOMY":case"UNSPECIFIED":return"economy"}}let r=["lite","economy","power"];function n(e){return r.some(t=>void 0!==e[t])?r.filter(t=>!e[t]?.length):void 0}function i(e){switch(e){case"free":return"FREE";case"lite":return"LITE";case"power":return"POWER";case"economy":return"ECONOMY"}}function l(e){return"TURBO"===e.modelProfile}let s=[...t.LITE_MODEL_SLUGS,...t.ECONOMY_MODEL_SLUGS,...t.POWER_MODEL_SLUGS];e.s(["getAgentConfigDiffForEffortClear",0,function(e){let t=s.find(t=>t===e);return void 0===t?null:{modelEfforts:{[t]:void 0},enableHighEffort:!1}},"getAgentConfigDiffForEffortPick",0,function(e,t){let o=s.find(t=>t===e);return void 0===o?null:{modelEfforts:{[o]:t},enableHighEffort:!1}},"getAgentConfigDiffForModelClear",0,function(e){switch(e){case"lite":return{liteModel:void 0};case"power":return{powerModel:void 0};case"economy":return{economyModel:void 0};case"free":return{}}},"getAgentConfigDiffForModelPick",0,function(e,o){if("lite"===e){let e=t.LITE_MODEL_SLUGS.find(e=>e===o);return void 0!==e?{liteModel:e}:null}if("economy"===e){let e=t.ECONOMY_MODEL_SLUGS.find(e=>e===o);return void 0!==e?{economyModel:e}:null}if("power"===e){let e=t.POWER_MODEL_SLUGS.find(e=>e===o);return void 0!==e?{powerModel:e}:null}return null},"getAgentConfigDiffForUIMode",0,function(e,t=!1){let o=i(e),r=t?{enableTurbo:!1}:{};return"lite"===e?{modelProfile:o,mode:"BUILD",...r}:{modelProfile:o,...r}},"getModelProfileForUIMode",0,i,"getSelectedEffortForModel",0,function(e,t){let o=s.find(t=>t===e);return void 0===o?void 0:t.modelEfforts?.[o]},"getSelectedModelForUIMode",0,function(e,t){switch(e){case"power":return t.powerModel;case"lite":return t.liteModel;case"economy":return t.economyModel;case"free":return}},"getUIModeFromAgentConfig",0,o,"hasServedAgentModels",0,function(e){return Object.values(e).some(e=>void 0!==e&&e.length>0)},"isAgentConfigTriggerOrange",0,function(e,t){return!t&&(l(e)||!0===e.enableHighEffort)},"isSelectedAgentModeUnavailable",0,function(e,t){return n(t)?.includes(o(e))??!1},"isTurboEnabled",0,l,"unavailableAgentModes",0,n])},640326,e=>{"use strict";var t=e.i(351623),o=e.i(721037),r=e.i(344480),n=e.i(975473),i=e.i(299020);let l={},s=t.gql`
    query ChateauAgentConfigPersistence($replId: String!) {
  getRepl(id: $replId) {
    ... on Repl {
      id
      currentUserSettings {
        visibleAgentModelsByMode {
          mode
          tiers {
            tier
            models {
              model
              label
              isDefault
              provider
              relativeCost
              recommendedEffort
              authorization {
                isAuthorized
                code
                message
              }
            }
          }
        }
        isBootstrapped
        ...UserReplSettingsFragment
      }
    }
  }
}
    ${o.UserReplSettingsFragmentFragmentDoc}`,a=t.gql`
    query ChateauAvailableAgentModelsRefresh($replId: String!) {
  getRepl(id: $replId) {
    ... on Repl {
      id
      currentUserSettings {
        visibleAgentModelsByMode {
          mode
          tiers {
            tier
            models {
              model
              label
              isDefault
              provider
              relativeCost
              recommendedEffort
              authorization {
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
}
    `,u=t.gql`
    mutation UpdateChateauAgentConfig($input: UpdateCurrentUserReplSettingsInput!) {
  updateCurrentUserReplSettings(input: $input) {
    __typename
    ... on UpdateCurrentUserReplSettingsPayload {
      userReplSettings {
        isBootstrapped
        ...UserReplSettingsFragment
      }
    }
    ... on UserError {
      message
    }
    ... on UnauthorizedError {
      message
    }
    ... on NotFoundError {
      message
    }
  }
}
    ${o.UserReplSettingsFragmentFragmentDoc}`;e.s(["useChateauAgentConfigPersistenceQuery",0,function(e){let t={...l,...e};return r.useQuery(s,t)},"useChateauAvailableAgentModelsRefreshLazyQuery",0,function(e){let t={...l,...e};return n.useLazyQuery(a,t)},"useUpdateChateauAgentConfigMutation",0,function(e){let t={...l,...e};return i.useMutation(u,t)}])},462669,e=>{"use strict";var t=e.i(389959),o=e.i(830675),r=e.i(908796),n=e.i(640326),i=e.i(11990),l=e.i(335421),s=e.i(211025),a=e.i(473072),u=e.i(29079),d=e.i(577671),p=e.i(46391),g=e.i(786864),f=e.i(349565),c=e.i(615593);let m={free:{},lite:f.LITE_MODEL_BY_GQL,economy:f.ECONOMY_MODEL_BY_GQL,power:f.POWER_MODEL_BY_GQL},E={[r.UserReplModelTier.Lite]:"lite",[r.UserReplModelTier.Economy]:"economy",[r.UserReplModelTier.Power]:"power",[r.UserReplModelTier.Free]:"free"},_={[r.AgentModelProvider.Anthropic]:"anthropic",[r.AgentModelProvider.Deepseek]:"deepseek",[r.AgentModelProvider.Google]:"google",[r.AgentModelProvider.Moonshot]:"moonshot",[r.AgentModelProvider.Openai]:"openai",[r.AgentModelProvider.Replit]:"replit",[r.AgentModelProvider.Zai]:"zai"},S={[r.UserReplAgentEffort.Low]:"low",[r.UserReplAgentEffort.Medium]:"medium",[r.UserReplAgentEffort.High]:"high",[r.UserReplAgentEffort.ExtraHigh]:"xhigh",[r.UserReplAgentEffort.Max]:"max"},M={low:r.UserReplAgentEffort.Low,medium:r.UserReplAgentEffort.Medium,high:r.UserReplAgentEffort.High,xhigh:r.UserReplAgentEffort.ExtraHigh,max:r.UserReplAgentEffort.Max},R={build:r.UserReplAgentMode.Build,design:r.UserReplAgentMode.Design};function U(e,t){let o=e.find(e=>e.mode===R[t]);return o?T(o.tiers):{}}function T(e){let t={};for(let o of e){let e=E[o.tier],r=m[e],n=o.models.flatMap(e=>{let t=r[e.model];return void 0!==t?[{slug:t,label:e.label,isDefault:e.isDefault,provider:_[e.provider],relativeCost:e.relativeCost,recommendedEffort:null==e.recommendedEffort?null:S[e.recommendedEffort],...e.authorization?.isAuthorized===!1?{authorization:e.authorization}:{}}]:[]});n.length>0&&(t[e]=n)}return t}function A(e){return e?.find(e=>e.isDefault)??e?.find(e=>l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e.slug))}function O(e){return A(e)??e?.[0]}function y(e,t){return t?e:Object.fromEntries(Object.entries(e).map(([e,t])=>{let o=(t??[]).filter(e=>!l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e.slug));return[e,t?.some(e=>e.isDefault&&l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e.slug))?function(e){if(e.some(e=>e.isDefault))return e;let t=e.find(e=>void 0===e.authorization);return void 0===t?e:e.map(e=>e===t?{...e,isDefault:!0}:e)}(o):o]}))}function h(e){return Object.fromEntries(Object.entries(e).map(([e,t])=>[e,t?.filter(e=>e.slug!==i.INTELLIGENT_AUTO_MODEL_SLUG)]))}function L(){let e=(0,a.default)(),[o]=(0,n.useChateauAvailableAgentModelsRefreshLazyQuery)({fetchPolicy:"network-only",errorPolicy:"none"});return(0,t.useCallback)(()=>o({variables:{replId:e}}),[o,e])}let b=["liteModel","economyModel","powerModel"];function v(e,t={}){if(!e)return{mode:"UNSPECIFIED",modelProfile:"UNSPECIFIED",enableTurbo:void 0,enableAutomatedTesting:void 0,enableHighEffort:void 0,liteModel:void 0,economyModel:void 0,powerModel:void 0,modelEfforts:void 0};let o=new Set(e.explicitFields),n=o.has(r.UserReplAgentSettingsField.IsTurboEnabled)&&null!=e.isTurboEnabled?e.isTurboEnabled:void 0,i=(()=>{if(!o.has(r.UserReplAgentSettingsField.ModelTier))return"UNSPECIFIED";switch(e.modelTier){case r.UserReplModelTier.Lite:return"LITE";case r.UserReplModelTier.Economy:return"ECONOMY";case r.UserReplModelTier.Power:return!0===n?"TURBO":"POWER";case r.UserReplModelTier.Free:return"FREE";default:return"UNSPECIFIED"}})(),l=o.has(r.UserReplAgentSettingsField.IsPlanModeEnabled)?!0===e.isPlanModeEnabled?"PLAN":"BUILD":"UNSPECIFIED",s=o.has(r.UserReplAgentSettingsField.IsAppTestingEnabled)&&null!=e.isAppTestingEnabled?e.isAppTestingEnabled:void 0,a=o.has(r.UserReplAgentSettingsField.IsHighEffortEnabled)&&null!=e.isHighEffortEnabled?e.isHighEffortEnabled:void 0,u=C(e),d=P(e),p=o.has(r.UserReplAgentSettingsField.LiteModel)&&null!=e.liteModel?D(f.LITE_MODEL_BY_GQL[e.liteModel],t.lite):void 0,g=o.has(r.UserReplAgentSettingsField.EconomyModel)&&null!=e.economyModel?D(f.ECONOMY_MODEL_BY_GQL[e.economyModel],t.economy):void 0;return{mode:l,modelProfile:i,enableTurbo:n,enableAutomatedTesting:s,enableHighEffort:a,intelligentAutoMode:u,autoPublish:d,liteModel:p,economyModel:g,powerModel:o.has(r.UserReplAgentSettingsField.PowerModel)&&null!=e.powerModel?D(f.POWER_MODEL_BY_GQL[e.powerModel],t.power):void 0,modelEfforts:(()=>{let t={};for(let o of e.modelEfforts){let e=f.MODEL_SLUG_BY_GQL[o.model];void 0!==e&&(t[e]=S[o.effort])}return Object.keys(t).length>0?t:void 0})()}}function C(e){if(e?.explicitFields.includes(r.UserReplAgentSettingsField.IsIntelligentAutoModeEnabled))return e.isIntelligentAutoModeEnabled??void 0}function P(e){if(e?.explicitFields.includes(r.UserReplAgentSettingsField.IsAutoPublishEnabled))return e.isAutoPublishEnabled??void 0}function D(e,t){return void 0===e||void 0===t||t.some(t=>t.slug===e&&void 0===t.authorization)?e:void 0}function I(e,t="build",o=!1,n=!0){if("design"===t){let t=function(e){let t={};if(void 0!==e.modelProfile)switch(e.modelProfile){case"LITE":t.modelTier=r.UserReplModelTier.Lite;break;case"ECONOMY":t.modelTier=r.UserReplModelTier.Economy;break;case"POWER":case"TURBO":t.modelTier=r.UserReplModelTier.Power;break;case"FREE":t.modelTier=r.UserReplModelTier.Free}return void 0!==e.enableAutomatedTesting&&(t.isAppTestingEnabled=e.enableAutomatedTesting),F(e,t),Object.keys(t).length>0?{designModeSettings:t}:{}}(e);return void 0!==e.intelligentAutoMode&&(t.isIntelligentAutoModeEnabled=e.intelligentAutoMode),void 0!==e.autoPublish&&(t.isAutoPublishEnabled=e.autoPublish),{agentSettings:t,taskSettings:{}}}let i={},l={};if(void 0!==e.modelProfile)switch(e.modelProfile){case"LITE":i.modelTier=o?null:r.UserReplModelTier.Lite,i.isPlanModeEnabled=!1,!1===n&&(l.isAutoApprovePlanEnabled=!1);break;case"ECONOMY":i.modelTier=r.UserReplModelTier.Economy;break;case"POWER":case"TURBO":i.modelTier=r.UserReplModelTier.Power;break;case"FREE":i.modelTier=r.UserReplModelTier.Free,!1===n&&(l.isAutoApprovePlanEnabled=!1)}return void 0!==e.mode&&("PLAN"===e.mode?i.isPlanModeEnabled=!0:"BUILD"===e.mode&&(i.isPlanModeEnabled=!1)),void 0!==e.enableAutomatedTesting&&(i.isAppTestingEnabled=e.enableAutomatedTesting),void 0!==e.enableTurbo&&(i.isTurboEnabled=e.enableTurbo),void 0!==e.enableHighEffort&&(i.isHighEffortEnabled=e.enableHighEffort),void 0!==e.intelligentAutoMode&&(i.isIntelligentAutoModeEnabled=e.intelligentAutoMode),void 0!==e.autoPublish&&(i.isAutoPublishEnabled=e.autoPublish),F(e,i),{agentSettings:i,taskSettings:l}}function F(e,t){if("liteModel"in e&&(t.liteModel=void 0===e.liteModel?null:f.GQL_BY_MODEL_SLUG[e.liteModel]),"economyModel"in e&&(t.economyModel=void 0===e.economyModel?null:f.GQL_BY_MODEL_SLUG[e.economyModel]),"powerModel"in e&&(t.powerModel=void 0===e.powerModel?null:f.GQL_BY_MODEL_SLUG[e.powerModel]),void 0!==e.modelEfforts){let o=Object.entries(e.modelEfforts);o.length>0&&(t.modelEfforts=o.map(([e,t])=>({model:f.GQL_BY_MODEL_SLUG[e],effort:void 0===t?null:M[t]})))}}e.s(["availableAgentModeOptions",0,function({options:e,canUpgradeToTurbo:t,isPerTierAutoModeAvailable:o}){let r=h(y(e,o));return t?r:Object.fromEntries(Object.entries(r).map(([e,t])=>[e,t?.filter(e=>void 0===e.authorization)]))},"filterAutoModeModelOptions",0,y,"getDefaultModelOption",0,O,"getModelDefaultsForSend",0,function(e,t={}){let{isEffortCapped:o=!1,isPerTierAutoModeAvailable:r=!0}=t,n=y(e,r),a={};for(let e of["lite","economy","power"]){let t=A(n[e])?.slug;void 0!==t&&(a[e]=t)}let u={},d={};if(r)for(let e of["economy","power"]){let t=a[e];void 0!==t&&l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(t)&&(d[e]=t)}else for(let t of["lite","economy","power"]){let o=a[t];e[t]?.some(e=>e.isDefault&&l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e.slug))&&void 0!==o&&(d[t]=o)}void 0!==a.power&&n.power?.every(e=>e.slug!==i.FABLE_5_MODEL_SLUG)&&(u.power=a.power);let p={};if(o)for(let e of Object.values(n))for(let t of e??[]){if(null===t.recommendedEffort)continue;let e=(0,s.clampToLadder)(t.recommendedEffort,s.CAPPED_EFFORT_LEVELS);e!==t.recommendedEffort&&(p[t.slug]=e)}return{defaultByTier:a,compatibilityPinByTier:u,...Object.keys(d).length>0&&{pinnedDefaultByTier:d},...Object.keys(p).length>0&&{cappedEffortByModel:p}}},"mapAvailableAgentModelsToOptions",0,T,"mapChateauConfigDiffToUserSettingsInput",0,I,"modeModelOptionsFor",0,function(e,t){let o={};for(let r of Object.values(E)){let n=e[r];if(void 0===n||0===n.length)continue;let i=(0,c.getSelectedModelForUIMode)(r,t);o[r]=n.find(e=>e.slug===i)??O(n)}return o},"modelOptionsForMode",0,U,"useAvailableAgentModels",0,function(e="build"){let o=(0,a.default)(),{isComplete:r}=(0,p.useUserReplSettingsBootstrapState)(),{data:i}=(0,n.useChateauAgentConfigPersistenceQuery)({variables:{replId:o},skip:!r}),l=i?.getRepl?.__typename==="Repl"?i.getRepl.currentUserSettings?.visibleAgentModelsByMode:void 0;return(0,t.useMemo)(()=>l?U(l,e):{},[l,e])},"useChateauAgentSettingsPersistence",0,function(e="build"){let i=(0,a.default)(),{isComplete:l}=(0,p.useUserReplSettingsBootstrapState)(),{shouldSeeFreemiumExperience:s}=(0,g.useShouldSeeFreemiumExperience)(i),{freeLiteTaskAutoApproval:c,loading:m}=(0,u.useAgentAuthorization)(),E=m||c?.isAuthorized!==!1,{data:_,loading:M,error:R}=(0,n.useChateauAgentConfigPersistenceQuery)({variables:{replId:i},skip:!l}),T=L(),A=(0,t.useRef)(Promise.resolve()),O=(0,t.useRef)(!1);(0,t.useEffect)(()=>{R&&!O.current&&(O.current=!0,o.captureException(R,{level:"warning",tags:{component:"useChateauAgentSettingsPersistence"},extra:{message:"falling back to default chateau agent config",replId:i}}))},[R,i]);let[y]=(0,n.useUpdateChateauAgentConfigMutation)(),h=_?.getRepl?.__typename==="Repl"?_.getRepl.currentUserSettings?.agentSettings??null:null,F=_?.getRepl?.__typename==="Repl"?_.getRepl.currentUserSettings?.visibleAgentModelsByMode:void 0,w=(0,t.useMemo)(()=>F?U(F,e):{},[F,e]),G=l&&(void 0!==_||void 0!==R);return{initialConfig:(0,t.useMemo)(()=>{if(G)return"design"===e?function(e,t={},o=null){let n={mode:"UNSPECIFIED",modelProfile:"UNSPECIFIED",enableTurbo:void 0,enableAutomatedTesting:void 0,enableHighEffort:void 0,intelligentAutoMode:C(o),autoPublish:P(o),liteModel:void 0,economyModel:void 0,powerModel:void 0,modelEfforts:void 0};if(!e)return n;let i=new Set(e.explicitFields),l=(()=>{if(!i.has(r.UserReplModeAgentSettingsField.ModelTier))return"UNSPECIFIED";switch(e.modelTier){case r.UserReplModelTier.Lite:return"LITE";case r.UserReplModelTier.Economy:return"ECONOMY";case r.UserReplModelTier.Power:return"POWER";case r.UserReplModelTier.Free:return"FREE";default:return"UNSPECIFIED"}})(),s=i.has(r.UserReplModeAgentSettingsField.IsAppTestingEnabled)&&null!=e.isAppTestingEnabled?e.isAppTestingEnabled:void 0,a=i.has(r.UserReplModeAgentSettingsField.LiteModel)&&null!=e.liteModel?D(f.LITE_MODEL_BY_GQL[e.liteModel],t.lite):void 0,u=i.has(r.UserReplModeAgentSettingsField.EconomyModel)&&null!=e.economyModel?D(f.ECONOMY_MODEL_BY_GQL[e.economyModel],t.economy):void 0,d=i.has(r.UserReplModeAgentSettingsField.PowerModel)&&null!=e.powerModel?D(f.POWER_MODEL_BY_GQL[e.powerModel],t.power):void 0,p=(()=>{let t={};for(let o of e.modelEfforts){let e=f.MODEL_SLUG_BY_GQL[o.model];void 0!==e&&(t[e]=S[o.effort])}return Object.keys(t).length>0?t:void 0})();return{...n,modelProfile:l,enableAutomatedTesting:s,liteModel:a,economyModel:u,powerModel:d,modelEfforts:p}}(h?.designModeSettings??null,w,h):v(h,w)},[G,e,w,h]),persistConfig:(0,t.useCallback)(async t=>{var r;let n=I(t,e,s,E);if(Object.values((r=n).agentSettings).some(e=>void 0!==e)||Object.values(r.taskSettings).some(e=>void 0!==e))try{let e=await y({variables:{input:{replId:i,agentSettings:n.agentSettings,taskSettings:n.taskSettings}},update:(e,t)=>{let o=t.data?.updateCurrentUserReplSettings;o?.__typename==="UpdateCurrentUserReplSettingsPayload"&&(0,d.writeCurrentUserReplSettingsToCache)({cache:e,replId:i,settings:o.userReplSettings})}}),r=e.data?.updateCurrentUserReplSettings;if(r?.__typename!=="UpdateCurrentUserReplSettingsPayload")return void o.captureMessage("updateCurrentUserReplSettings returned non-payload",{extra:{replId:i,typename:r?.__typename,message:r&&"message"in r?r.message:void 0}});b.some(e=>e in t&&void 0===t[e])&&(A.current=A.current.then(T).catch(()=>void 0))}catch(e){o.captureException(e,{extra:{message:"Error persisting chateau agent config",replId:i,configDiff:t}})}},[y,i,T,e,s,E]),loading:!l||M||m}},"useRawBuildAgentConfig",0,function(){let e=(0,a.default)(),{isComplete:o}=(0,p.useUserReplSettingsBootstrapState)(),{data:r}=(0,n.useChateauAgentConfigPersistenceQuery)({variables:{replId:e},skip:!o}),i=r?.getRepl?.__typename==="Repl"?r.getRepl.currentUserSettings:void 0;return(0,t.useMemo)(()=>v(i?.agentSettings??null,i?.visibleAgentModelsByMode?U(i.visibleAgentModelsByMode,"build"):{}),[i])},"useRefreshAvailableAgentModels",0,L,"withoutIntelligentAutoOption",0,h])},721037,e=>{"use strict";var t=e.i(351623),o=e.i(344480);e.i(975473);var r=e.i(299020);let n={},i=t.gql`
    fragment UserReplSettingsFragment on UserReplSettings {
  agentSettings {
    explicitFields
    modelTier
    liteModel
    economyModel
    powerModel
    modelEfforts {
      model
      effort
    }
    isTurboEnabled
    isHighEffortEnabled
    isIntelligentAutoModeEnabled
    isPlanModeEnabled
    isAppTestingEnabled
    isAutoPublishEnabled
    isCodeOptimizationEnabled
    isAutoCheckpointingEnabled
    designModeSettings {
      explicitFields
      modelTier
      liteModel
      economyModel
      powerModel
      modelEfforts {
        model
        effort
      }
      isAppTestingEnabled
    }
  }
  taskSettings {
    explicitFields
    isAutoApprovePlanEnabled
    isAutoMergeEnabled
  }
}
    `,l=t.gql`
    fragment CurrentUserReplSettingsCacheWrite on Repl {
  id
  currentUserSettings {
    ...UserReplSettingsFragment
  }
}
    ${i}`,s=t.gql`
    query CurrentUserReplSettings($replId: String!) {
  currentUser {
    id
  }
  getRepl(id: $replId) {
    ... on Repl {
      id
      currentUserSettings {
        ...UserReplSettingsFragment
      }
    }
  }
}
    ${i}`,a=t.gql`
    mutation UpdateCurrentUserReplSettings($input: UpdateCurrentUserReplSettingsInput!) {
  updateCurrentUserReplSettings(input: $input) {
    __typename
    ... on UpdateCurrentUserReplSettingsPayload {
      userReplSettings {
        ...UserReplSettingsFragment
      }
    }
    ... on UserError {
      message
    }
    ... on UnauthorizedError {
      message
    }
    ... on NotFoundError {
      message
    }
  }
}
    ${i}`;e.s(["CurrentUserReplSettingsCacheWriteFragmentDoc",0,l,"UserReplSettingsFragmentFragmentDoc",0,i,"useCurrentUserReplSettingsQuery",0,function(e){let t={...n,...e};return o.useQuery(s,t)},"useUpdateCurrentUserReplSettingsMutation",0,function(e){let t={...n,...e};return r.useMutation(a,t)}])},577671,e=>{"use strict";var t=e.i(389959),o=e.i(830675),r=e.i(721037);function n({cache:e,replId:t,settings:o}){let i=e.identify({__typename:"Repl",id:t});e.writeFragment({id:i,fragment:r.CurrentUserReplSettingsCacheWriteFragmentDoc,fragmentName:"CurrentUserReplSettingsCacheWrite",data:{__typename:"Repl",id:t,currentUserSettings:o}}),e.modify({id:i,fields:{currentUserSettings:e=>({...e,...o})}})}e.s(["useCurrentUserReplSettings",0,function(e){let{data:i,loading:l}=(0,r.useCurrentUserReplSettingsQuery)({variables:{replId:e}}),[s]=(0,r.useUpdateCurrentUserReplSettingsMutation)(),a=i?.getRepl?.__typename==="Repl"?i.getRepl:null,u=a?.currentUserSettings?.agentSettings??null,d=a?.currentUserSettings?.taskSettings??null;return{isResolved:null!==a,loading:l,agentSettings:u,taskSettings:d,updateSettings:(0,t.useCallback)(async t=>{try{let r=await s({variables:{input:{replId:e,...t}},update:(t,o)=>{let r=o.data?.updateCurrentUserReplSettings;r?.__typename==="UpdateCurrentUserReplSettingsPayload"&&n({cache:t,replId:e,settings:r.userReplSettings})}}),i=r.data?.updateCurrentUserReplSettings;if(i?.__typename==="UpdateCurrentUserReplSettingsPayload")return!0;return o.captureMessage("updateCurrentUserReplSettings returned non-payload",{extra:{replId:e,typename:i?.__typename,message:i&&"message"in i?i.message:void 0}}),!1}catch(r){return o.captureException(r,{extra:{message:"Error updating current user repl settings",replId:e,input:t}}),!1}},[e,s])}},"writeCurrentUserReplSettingsToCache",0,n])},734135,e=>{"use strict";var t=e.i(351623),o=e.i(344480);e.i(975473);let r={},n=t.gql`
    query GetShouldSeeFreemiumExperience($replId: String!) {
  getRepl(id: $replId) {
    __typename
    ... on Repl {
      id
      authorizations {
        viewFreemiumExperience {
          isAuthorized
        }
      }
    }
  }
}
    `;e.s(["useGetShouldSeeFreemiumExperienceQuery",0,function(e){let t={...r,...e};return o.useQuery(n,t)}])},786864,e=>{"use strict";var t=e.i(734135);e.s(["useShouldSeeFreemiumExperience",0,function(e,o){let{data:r,loading:n,error:i,refetch:l}=(0,t.useGetShouldSeeFreemiumExperienceQuery)({variables:{replId:e},skip:o?.skip});return{shouldSeeFreemiumExperience:r?.getRepl.__typename==="Repl"&&r.getRepl.authorizations.viewFreemiumExperience.isAuthorized,loading:n,error:i,refetch:l}}])},925712,e=>{"use strict";var t=e.i(351623),o=e.i(721037),r=e.i(344480);e.i(975473);var n=e.i(299020);let i={},l=t.gql`
    fragment UserReplSettingsBootstrapFragment on UserReplSettings {
  isBootstrapped
  ...UserReplSettingsFragment
}
    ${o.UserReplSettingsFragmentFragmentDoc}`,s=t.gql`
    query UserReplSettingsBootstrap($replId: String!) {
  currentUser {
    id
  }
  getRepl(id: $replId) {
    ... on Repl {
      id
      currentUserSettings {
        ...UserReplSettingsBootstrapFragment
      }
    }
  }
}
    ${l}`,a=t.gql`
    mutation BootstrapCurrentUserReplSettings($input: BootstrapCurrentUserReplSettingsInput!) {
  bootstrapCurrentUserReplSettings(input: $input) {
    __typename
    ... on BootstrapCurrentUserReplSettingsPayload {
      didBootstrap
      userReplSettings {
        ...UserReplSettingsBootstrapFragment
      }
    }
    ... on UserError {
      message
    }
    ... on UnauthorizedError {
      message
    }
    ... on NotFoundError {
      message
    }
  }
}
    ${l}`;e.s(["useBootstrapCurrentUserReplSettingsMutation",0,function(e){let t={...i,...e};return n.useMutation(a,t)},"useUserReplSettingsBootstrapQuery",0,function(e){let t={...i,...e};return r.useQuery(s,t)}])},567314,e=>{"use strict";var t=e.i(389959),o=e.i(830675),r=e.i(925712),n=e.i(577671);let i=[250,1e3];async function l({attempt:e,bootstrap:t,replId:r}){try{let e=await t({variables:{input:{replId:r}}}),n=e.data?.bootstrapCurrentUserReplSettings;if(n?.__typename==="BootstrapCurrentUserReplSettingsPayload")return;o.captureMessage("bootstrapCurrentUserReplSettings returned non-payload",{extra:{replId:r,typename:n?.__typename,message:n&&"message"in n?n.message:void 0}})}catch(s){let o=i[e];if(null!=o){var n;return await (n=o,new Promise(e=>{setTimeout(e,n)})),l({attempt:e+1,bootstrap:t,replId:r})}throw s}}e.s(["useUserReplSettingsBootstrap",0,function(e,i){let s=(0,t.useRef)(new Set),[a,u]=(0,t.useState)(()=>({isComplete:!1,isBootstrapping:!1})),{data:d,loading:p}=(0,r.useUserReplSettingsBootstrapQuery)({variables:{replId:i??""},skip:!e||null===i}),[g]=(0,r.useBootstrapCurrentUserReplSettingsMutation)({update(e,t){let o=t.data?.bootstrapCurrentUserReplSettings;o?.__typename!=="BootstrapCurrentUserReplSettingsPayload"||null!==i&&(0,n.writeCurrentUserReplSettingsToCache)({cache:e,replId:i,settings:o.userReplSettings})}}),f=d?.getRepl.__typename==="Repl"?d.getRepl:null,c=d?.currentUser!=null,m=null!=f,E=f?.currentUserSettings?.isBootstrapped===!0;return(0,t.useEffect)(()=>{if(p)return void u({isComplete:!1,isBootstrapping:!1});if(null===i||!c||!m||E)return void u({isComplete:!0,isBootstrapping:!1});if(s.current.has(i))return;s.current.add(i);let e=!1;return u({isComplete:!1,isBootstrapping:!0}),(async()=>{try{await l({attempt:0,bootstrap:g,replId:i})}catch(e){o.captureException(e,{extra:{message:"Error bootstrapping current user repl settings",replId:i}})}finally{e||u({isComplete:!0,isBootstrapping:!1})}})(),()=>{e=!0}},[g,c,m,E,p,i]),a}])},46391,e=>{"use strict";var t=e.i(276385),o=e.i(389959),r=e.i(567314);let n=(0,o.createContext)({isComplete:!0,isBootstrapping:!1});e.s(["UserReplSettingsBootstrapProvider",0,function({children:e,enabled:i=!0,replId:l}){let s=(0,r.useUserReplSettingsBootstrap)(i,l),a=(0,o.useMemo)(()=>({isComplete:s.isComplete,isBootstrapping:s.isBootstrapping}),[s.isComplete,s.isBootstrapping]);return(0,t.jsx)(n.Provider,{value:a,children:e})},"useUserReplSettingsBootstrapState",0,function(){return(0,o.useContext)(n)}])},301128,e=>{"use strict";var t=e.i(960933);let o=t.Type.Union([t.Type.Object({type:t.Type.Literal("recent")},{additionalProperties:!1}),t.Type.Object({type:t.Type.Literal("pinned")},{additionalProperties:!1}),t.Type.Object({type:t.Type.Literal("custom"),sectionId:t.Type.String({pattern:"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"})},{additionalProperties:!1})]);e.s(["SidebarDestinationSchema",0,o])},306229,e=>{"use strict";var t=e.i(960933),o=e.i(862927),r=e.i(598986),n=e.i(301128);let i="replit:home-sidebar-destination",l=t.Type.Object({intentId:t.Type.String(),userId:t.Type.Number(),orgId:t.Type.Union([t.Type.String(),t.Type.Null()]),destination:n.SidebarDestinationSchema,createdAt:t.Type.Number()});e.s(["clearHomeSidebarDestination",0,function(){r.default.remove(i)},"consumeHomeSidebarDestination",0,function(e){let t=r.default.get(i);void 0!==e&&o.Value.Check(l,t)&&t.intentId===e&&r.default.remove(i)},"readHomeSidebarDestination",0,function(e,t){let n=r.default.get(i);if(!(!o.Value.Check(l,n)||n.userId!==e||n.orgId!==(t??null)||Date.now()-n.createdAt>18e5))return{intentId:n.intentId,destination:n.destination}},"setHomeSidebarDestination",0,function(e,t,o){r.default.set(i,{intentId:crypto.randomUUID(),userId:e,orgId:t??null,destination:o,createdAt:Date.now()})}])},11990,e=>{"use strict";e.s(["FABLE_5_MODEL_SLUG",0,"claude-fable-5","INTELLIGENT_AUTO_MODEL_SLUG",0,"intelligent-auto","TURBO_CARRYOVER_MODEL_SLUG",0,"claude-5-opus-fast"])},335421,e=>{"use strict";let t=["free-auto"],o=["kimi-k2-7","gpt-5-6-luna","gemini-3-5-flash","deepseek-v4-flash"],r=["claude-4-6-sonnet","gpt-5-6-luna","gpt-5-6-luna-fast","claude-5-sonnet","gpt-5-6-terra","gemini-3-1-pro","glm-5-2","economy-auto","design-economy-auto"],n=["claude-fable-5","claude-fable-5-1","power-auto","intelligent-auto","design-power-auto","claude-4-8-opus","claude-4-8-opus-fast","claude-5-opus","claude-5-opus-fast","kimi-k3","gpt-5-6-terra-fast","gpt-5-6-sol","gpt-6-astra"],i={free:t[0],lite:o[0],economy:r[0],power:n[0]},l=new Set(["free-auto","economy-auto","design-economy-auto","power-auto","intelligent-auto","design-power-auto"]),s=new Set(["free-auto","economy-auto","design-economy-auto","power-auto","design-power-auto"]);e.s(["USER_REPL_DEFAULT_MODELS_BY_TIER",0,i,"USER_REPL_ECONOMY_MODELS",0,r,"USER_REPL_EFFORT_LOCKED_SLUGS",0,l,"USER_REPL_FAST_MODE_MODEL_SLUGS",0,["claude-4-8-opus-fast","claude-5-opus-fast"],"USER_REPL_FREE_MODELS",0,t,"USER_REPL_LITE_MODELS",0,o,"USER_REPL_MODEL_BADGES",0,{},"USER_REPL_MODEL_LABELS",0,{"free-auto":"Auto","kimi-k2-7":"Kimi K2.7","gpt-5-6-luna":"GPT-5.6 Luna","gemini-3-5-flash":"Gemini 3.5 Flash","deepseek-v4-flash":"DeepSeek V4 Flash","claude-4-6-sonnet":"Claude Sonnet 4.6","gpt-5-6-luna-fast":"GPT-5.6 Luna Fast","claude-5-sonnet":"Claude Sonnet 5","gpt-5-6-terra":"GPT-5.6 Terra","gemini-3-1-pro":"Gemini 3.1 Pro","glm-5-2":"GLM 5.2","economy-auto":"Auto","design-economy-auto":"Auto","claude-fable-5":"Claude Fable 5","claude-fable-5-1":"Claude Fable 5.1","power-auto":"Auto","intelligent-auto":"Intelligence","design-power-auto":"Auto","claude-4-8-opus":"Claude Opus 4.8","claude-4-8-opus-fast":"Claude Opus 4.8 Fast","claude-5-opus":"Claude Opus 5","claude-5-opus-fast":"Claude Opus 5 Fast","kimi-k3":"Kimi K3","gpt-5-6-terra-fast":"GPT-5.6 Terra Fast","gpt-5-6-sol":"GPT-5.6 Sol","gpt-6-astra":"GPT-6 Astra"},"USER_REPL_MODEL_PROVIDERS",0,{"free-auto":"replit","kimi-k2-7":"moonshot","gpt-5-6-luna":"openai","gemini-3-5-flash":"google","deepseek-v4-flash":"deepseek","claude-4-6-sonnet":"anthropic","gpt-5-6-luna-fast":"openai","claude-5-sonnet":"anthropic","gpt-5-6-terra":"openai","gemini-3-1-pro":"google","glm-5-2":"zai","economy-auto":"replit","design-economy-auto":"replit","claude-fable-5":"anthropic","claude-fable-5-1":"anthropic","power-auto":"replit","intelligent-auto":"replit","design-power-auto":"replit","claude-4-8-opus":"anthropic","claude-4-8-opus-fast":"anthropic","claude-5-opus":"anthropic","claude-5-opus-fast":"anthropic","kimi-k3":"moonshot","gpt-5-6-terra-fast":"openai","gpt-5-6-sol":"openai","gpt-6-astra":"openai"},"USER_REPL_POWER_MODELS",0,n,"USER_REPL_TIER_AUTO_MODEL_SLUGS",0,s])}]);

//# debugId=a6ab4c20-aba2-d7c6-fbbe-44bfa33c1e59
//# sourceMappingURL=12bnn4unjakol.js.map