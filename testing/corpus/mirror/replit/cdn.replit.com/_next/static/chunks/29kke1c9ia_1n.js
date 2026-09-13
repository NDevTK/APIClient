;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="1870298d-9512-80bc-cebe-49016a2aacba")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,411719,(e,t,r)=>{"use strict";Object.defineProperty(r,"__esModule",{value:!0}),Object.defineProperty(r,"LoadableContext",{enumerable:!0,get:function(){return o}});let o=e.r(2879)._(e.r(389959)).default.createContext(null)},46147,(e,t,r)=>{"use strict";Object.defineProperty(r,"__esModule",{value:!0}),Object.defineProperty(r,"default",{enumerable:!0,get:function(){return g}});let o=e.r(2879)._(e.r(389959)),i=e.r(411719),n=[],l=[],s=!1;function a(e){let t=e(),r={loading:!0,loaded:null,error:null};return r.promise=t.then(e=>(r.loading=!1,r.loaded=e,e)).catch(e=>{throw r.loading=!1,r.error=e,e}),r}class u{constructor(e,t){this._loadFn=e,this._opts=t,this._callbacks=new Set,this._delay=null,this._timeout=null,this.retry()}promise(){return this._res.promise}retry(){this._clearTimeouts(),this._res=this._loadFn(this._opts.loader),this._state={pastDelay:!1,timedOut:!1};let{_res:e,_opts:t}=this;e.loading&&("number"==typeof t.delay&&(0===t.delay?this._state.pastDelay=!0:this._delay=setTimeout(()=>{this._update({pastDelay:!0})},t.delay)),"number"==typeof t.timeout&&(this._timeout=setTimeout(()=>{this._update({timedOut:!0})},t.timeout))),this._res.promise.then(()=>{this._update({}),this._clearTimeouts()}).catch(e=>{this._update({}),this._clearTimeouts()}),this._update({})}_update(e){this._state={...this._state,error:this._res.error,loaded:this._res.loaded,loading:this._res.loading,...e},this._callbacks.forEach(e=>e())}_clearTimeouts(){clearTimeout(this._delay),clearTimeout(this._timeout)}getCurrentValue(){return this._state}subscribe(e){return this._callbacks.add(e),()=>{this._callbacks.delete(e)}}}function d(t){return function(t,r){let a=Object.assign({loader:null,loading:null,delay:200,timeout:null,webpack:null,modules:null},r),d=null;function p(){if(!d){let e=new u(t,a);d={getCurrentValue:e.getCurrentValue.bind(e),subscribe:e.subscribe.bind(e),retry:e.retry.bind(e),promise:e.promise.bind(e)}}return d.promise()}if("u"<typeof window&&n.push(p),!s&&"u">typeof window){let t=a.webpack&&"function"==typeof e.t.resolveWeak?a.webpack():a.modules;t&&l.push(e=>{for(let r of t)if(e.includes(r))return p()})}function g(e,t){let r;p(),(r=o.default.useContext(i.LoadableContext))&&Array.isArray(a.modules)&&a.modules.forEach(e=>{r(e)});let n=o.default.useSyncExternalStore(d.subscribe,d.getCurrentValue,d.getCurrentValue);return o.default.useImperativeHandle(t,()=>({retry:d.retry}),[]),o.default.useMemo(()=>{var t;return n.loading||n.error?o.default.createElement(a.loading,{isLoading:n.loading,pastDelay:n.pastDelay,timedOut:n.timedOut,error:n.error,retry:d.retry}):n.loaded?o.default.createElement((t=n.loaded)&&t.default?t.default:t,e):null},[e,n])}return g.preload=()=>p(),g.displayName="LoadableComponent",o.default.forwardRef(g)}(a,t)}function p(e,t){let r=[];for(;e.length;){let o=e.pop();r.push(o(t))}return Promise.all(r).then(()=>{if(e.length)return p(e,t)})}d.preloadAll=()=>new Promise((e,t)=>{p(n).then(e,t)}),d.preloadReady=(e=[])=>new Promise(t=>{let r=()=>(s=!0,t());p(l,e).then(r,r)}),"u">typeof window&&(window.__NEXT_PRELOADREADY=d.preloadReady);let g=d},493092,(e,t,r)=>{"use strict";Object.defineProperty(r,"__esModule",{value:!0});var o={default:function(){return p},noSSR:function(){return d}};for(var i in o)Object.defineProperty(r,i,{enumerable:!0,get:o[i]});let n=e.r(2879),l=e.r(478902);e.r(389959);let s=n._(e.r(46147)),a="u"<typeof window;function u(e){return{default:e?.default||e}}function d(e,t){if(delete t.webpack,delete t.modules,!a)return e(t);let r=t.loading;return()=>(0,l.jsx)(r,{error:null,isLoading:!0,pastDelay:!1,timedOut:!1})}function p(e,t){let r=s.default,o={loading:({error:e,isLoading:t,pastDelay:r})=>null};e instanceof Promise?o.loader=()=>e:"function"==typeof e?o.loader=e:"object"==typeof e&&(o={...o,...e});let i=(o={...o,...t}).loader;return(o.loadableGenerated&&(o={...o,...o.loadableGenerated},delete o.loadableGenerated),"boolean"!=typeof o.ssr||o.ssr)?r({...o,loader:()=>null!=i?i().then(u):Promise.resolve(u(()=>null))}):(delete o.webpack,delete o.modules,d(r,o))}("function"==typeof r.default||"object"==typeof r.default&&null!==r.default)&&void 0===r.default.__esModule&&(Object.defineProperty(r.default,"__esModule",{value:!0}),Object.assign(r.default,r),t.exports=r.default)},196786,(e,t,r)=>{t.exports=e.r(493092)},211025,e=>{"use strict";let t=["low","medium","high","xhigh","max"],r=["low","medium","high"];e.s(["CAPPED_EFFORT_LEVELS",0,r,"EFFORT_LEVELS",0,t,"EFFORT_LEVEL_LABELS",0,{low:{label:"Low",labelId:"workspace.agentSettingsEffortNameLow"},medium:{label:"Medium",labelId:"workspace.agentSettingsEffortNameMedium"},high:{label:"High",labelId:"workspace.agentSettingsEffortNameHigh"},xhigh:{label:"Extra High",labelId:"workspace.agentSettingsEffortNameExtraHigh"},max:{label:"Max",labelId:"workspace.agentSettingsEffortNameMax"}},"clampToLadder",0,function(e,t){return t.includes(e)?e:t[t.length-1]},"effortLevelsFor",0,function(e){return e?r:t}])},621738,e=>{"use strict";e.s(["DEFAULT_CHATEAU_AGENT_CONFIG",0,{mode:"UNSPECIFIED",modelProfile:"UNSPECIFIED",enableTurbo:void 0,enableAutomatedTesting:void 0,enableHighEffort:void 0,autoApprovePlan:void 0,autoMerge:void 0,autoPublish:void 0,intelligentAutoMode:void 0,liteModel:void 0,economyModel:void 0,powerModel:void 0,modelEfforts:void 0},"applyAgentConfigDiff",0,function(e,t){let r={...e,...t};if(!("modelEfforts"in t)||void 0===t.modelEfforts)return r;let o={...e.modelEfforts};for(let[e,r]of Object.entries(t.modelEfforts))void 0===r?delete o[e]:o[e]=r;return r.modelEfforts=Object.keys(o).length>0?o:void 0,r}])},42585,e=>{"use strict";var t=e.i(351623),r=e.i(319801),o=e.i(260666),i=e.i(218766),n=e.i(344480);e.i(975473);let l={},s=t.gql`
    fragment ReplsListRepl on Repl {
  id
  title
  lastMutated
  firstOpened
  lastOpened
  timeCreated
  isCurrentUserStarred
  authorizations {
    star {
      isAuthorized
    }
  }
  latestAgentScreenshotUrl
  latestAgentRunSummary
  user {
    id
    username
    image
  }
  org {
    id
  }
  ...ReplLinkRepl
  ...DeployRepl
  ...ReplAgentStatusRepl
}
    ${r.ReplLinkReplFragmentDoc}
${o.DeployReplFragmentDoc}
${i.ReplAgentStatusReplFragmentDoc}`,a=t.gql`
    query SidebarConversationCreator {
  currentUser {
    id
    username
    image
  }
}
    `,u=t.gql`
    query ReplsList($input: CurrentUserReplsInput!) {
  currentUser {
    id
    repls(input: $input) {
      __typename
      ... on ReplConnection {
        items {
          id
          ...ReplsListRepl
        }
        pageInfo {
          hasNextPage
          nextCursor
        }
      }
      ... on Error {
        message
      }
    }
  }
}
    ${s}`,d=t.gql`
    query SidebarSelectedRepl($replId: String!) {
  currentUser {
    id
  }
  getRepl(id: $replId) {
    __typename
    ... on Repl {
      ...ReplsListRepl
    }
  }
}
    ${s}`;e.s(["ReplsListReplFragmentDoc",0,s,"useReplsListQuery",0,function(e){let t={...l,...e};return n.useQuery(u,t)},"useSidebarConversationCreatorQuery",0,function(e){let t={...l,...e};return n.useQuery(a,t)},"useSidebarSelectedReplQuery",0,function(e){let t={...l,...e};return n.useQuery(d,t)}])},758423,e=>{"use strict";e.s(["insertReplIntoSidebarList",0,function({cache:e,userId:t,repl:r}){let o=e.identify({__typename:"CurrentUser",id:t});if(void 0===o)return!1;let i=r.org?.id??null,n=!1;return e.modify({id:o,fields:{repls(e,{storeFieldName:t,readField:o,toReference:l}){if("ReplConnection"!==e.__typename)return e;let s=function(e){let t=e.indexOf("(");if(-1===t||!e.endsWith(")"))return null;try{return JSON.parse(e.slice(t+1,-1))}catch{return null}}(t);if(null===s||s.input?.cursor||(s.input?.orgId??null)!==i||(null===i?s.input?.filters?.excludeShared!==!0:s.input?.filters?.interactedOnly!==!0))return e;let a=e.items??[];if(a.some(e=>o("id",e)===r.id))return e;let u=l({__typename:"Repl",id:r.id});return void 0===u?e:(n=!0,{...e,items:[u,...a]})}}}),n}])},319801,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment ReplLinkRepl on Repl {
  id
  url
  nextPagePathname
}
    `;e.s(["ReplLinkReplFragmentDoc",0,r])},921125,e=>{"use strict";var t=e.i(276385),r=e.i(36454),o=e.i(569910),i=e.i(568087);let n=window.location.origin,l=(e={})=>{let t={};switch(e.initialPaneType){case"deployments":t.deploymentPane="true";break;case"securityScan":t.securityScanPane="true";break;case void 0:break;default:(0,o.default)(e.initialPaneType)}return e.enterInAgentMode&&(t.enterInAgentMode="true"),e.autorun&&(t.run="true"),e.initialQueuedPromptKey&&(t.initialQueuedPromptKey=e.initialQueuedPromptKey),e.initialEditorMode&&(t.initialEditorMode=e.initialEditorMode),t},s=(e,t={})=>({href:{pathname:e.nextPagePathname,query:((e,t={})=>({replId:e.id,...l(t)}))(e,t)},as:{pathname:e.url,query:l(t),...t.suppressIosUniversalLinkBanner&&{hash:i.NO_UNIVERSAL_LINKS_FRAGMENT}}}),a=(e,t={})=>{let r=new URLSearchParams(l(t)),o=r.size>=1?`?${r.toString()}`:"",n=t.suppressIosUniversalLinkBanner?i.NO_UNIVERSAL_LINKS_HASH:"";return`${e.url}${o}${n}`};e.s(["default",0,({repl:e,children:o,options:i,...n})=>(0,t.jsx)(r.default,{...n,...s(e,i),children:o}),"replLinkFullUrl",0,(e,t={})=>`${n}${a(e,t)}`,"replLinkPath",0,a,"replLinkProps",0,s,"replViewLinkProps",0,e=>({as:e.org?.id?{pathname:`${e.url}/view`}:{pathname:e.url,query:{v:"1"}},href:{pathname:"/replView",query:{replId:e.id}}})])},394701,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var o=e.i(299020);let i={},n=t.gql`
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
    `;e.s(["TransferReplBetweenWorkspacesDialogReplFragmentDoc",0,n,"useTransferReplBetweenWorkspacesDialogDestinationsQuery",0,function(e){let t={...i,...e};return r.useQuery(l,t)},"useTransferReplBetweenWorkspacesDialogTransferMutation",0,function(e){let t={...i,...e};return o.useMutation(s,t)}])},781258,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var o=e.i(299020);let i={},n=t.gql`
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
    `;e.s(["TransferReplToOrgDialogReplFragmentDoc",0,n,"useTransferReplToOrgDialogOrgsQuery",0,function(e){let t={...i,...e};return r.useQuery(l,t)},"useTransferReplToOrgDialogTransferMutation",0,function(e){let t={...i,...e};return o.useMutation(s,t)}])},618876,e=>{"use strict";var t=e.i(960933),r=e.i(335421),o=e.i(211025),i=e.i(621738),n=e.i(987997),l=e.i(489859),s=e.i(615593);let a={build:"creation-agent-model-picks",design:"creation-agent-design-model-picks"};function u(e,t){return(0,n.creationAgentStorageKey)(a[e],t)}let d=t.Type.Object({liteModel:t.Type.Optional(t.Type.String()),economyModel:t.Type.Optional(t.Type.String()),powerModel:t.Type.Optional(t.Type.String()),modelEfforts:t.Type.Optional(t.Type.Record(t.Type.String(),t.Type.String()))});function p(e){let t={};for(let[i,n]of(void 0!==e.liteModel&&(t=g(t,(0,s.getAgentConfigDiffForModelPick)("lite",e.liteModel))),void 0!==e.economyModel&&(t=g(t,(0,s.getAgentConfigDiffForModelPick)("economy",e.economyModel))),void 0!==e.powerModel&&(t=g(t,(0,s.getAgentConfigDiffForModelPick)("power",e.powerModel))),Object.entries(e.modelEfforts??{}))){var r;r=n,o.EFFORT_LEVELS.includes(r)&&(t=g(t,(0,s.getAgentConfigDiffForEffortPick)(i,n)))}return t}function g(e,t){if(null===t)return e;let r=(0,i.applyAgentConfigDiff)({...i.DEFAULT_CHATEAU_AGENT_CONFIG,...e},t);return{...void 0!==r.liteModel&&{liteModel:r.liteModel},...void 0!==r.economyModel&&{economyModel:r.economyModel},...void 0!==r.powerModel&&{powerModel:r.powerModel},...void 0!==r.modelEfforts&&{modelEfforts:r.modelEfforts}}}e.s(["applyCreationAgentModelPicks",0,g,"capCreationAgentModelPicks",0,function(e,t){return t&&void 0!==e.modelEfforts?{...e,modelEfforts:Object.fromEntries(Object.entries(e.modelEfforts).map(([e,t])=>[e,(0,o.clampToLadder)(t,o.CAPPED_EFFORT_LEVELS)]))}:e},"creationAgentModelPicksFromStored",0,p,"filterCreationAgentModelPicks",0,function(e,t,o=!0){let i=Object.values(t).flatMap(e=>e??[]),n=Object.fromEntries(Object.entries(e.modelEfforts??{}).filter(([e])=>i.some(t=>t.slug===e&&null!==t.recommendedEffort&&void 0===t.authorization))),l=e=>o||!r.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e);return{...void 0!==e.liteModel&&l(e.liteModel)&&t.lite?.some(t=>t.slug===e.liteModel&&void 0===t.authorization)&&{liteModel:e.liteModel},...void 0!==e.economyModel&&l(e.economyModel)&&t.economy?.some(t=>t.slug===e.economyModel&&void 0===t.authorization)&&{economyModel:e.economyModel},...void 0!==e.powerModel&&l(e.powerModel)&&t.power?.some(t=>t.slug===e.powerModel&&void 0===t.authorization)&&{powerModel:e.powerModel},...Object.keys(n).length>0&&{modelEfforts:n}}},"readCreationAgentModelPicks",0,function(e,t){let r=l.default.get(u(e,t),d);return null===r?{}:p(r)},"writeCreationAgentModelPicks",0,function(e,t,r){l.default.set(u(t,r),e)}])},987997,e=>{"use strict";e.s(["creationAgentStorageKey",0,function(e,t){return void 0===t?e:`${e}:org:${t}`}])},328789,e=>{"use strict";var t=e.i(908796),r=e.i(11990),o=e.i(462669);let i=[300,300,300,300];async function n({replId:e,modelProfile:r,isPlanModeEnabled:o,isModelProfileChosen:l=!0,persist:s,onError:a,retryDelaysMs:u=i}){let d=function(e,r=!0){if(r)switch(e){case"FREE":return t.UserReplModelTier.Free;case"LITE":return t.UserReplModelTier.Lite;case"ECONOMY":return t.UserReplModelTier.Economy;case"POWER":case"TURBO":return t.UserReplModelTier.Power;case"UNSPECIFIED":case void 0:return}}(r,l);if(l&&void 0===d)return!1;for(let t=0;;t+=1){try{let t=await s(e,d,o);if("persisted"===t)return!0}catch(e){return a(e instanceof Error?e:Error(String(e))),!1}let r=u[t];if(void 0===r)return a(Error("Could not find the new repl to persist Agent mode")),!1;await new Promise(e=>{setTimeout(e,r)})}}e.s(["PERSIST_AGENT_MODE_SETTINGS_TIMEOUT_MS",0,5e3,"getCreationModePickerAgentSettings",0,function({modelTier:e,isPlanModeEnabled:t,modelPicks:i,modelSettingsMode:n,intelligentAutoMode:l}){let{designModeSettings:s,...a}=(0,o.mapChateauConfigDiffToUserSettingsInput)(i,n).agentSettings,u=void 0===e?{}:{modelTier:e},d=!0===l&&i.powerModel===r.INTELLIGENT_AUTO_MODEL_SLUG?(0,o.mapChateauConfigDiffToUserSettingsInput)({powerModel:i.powerModel},"build").agentSettings:{},p=void 0===l?{}:(0,o.mapChateauConfigDiffToUserSettingsInput)({intelligentAutoMode:l},"build").agentSettings,g={...u,...d,...s??{}};return{...u,isPlanModeEnabled:t,...d,...p,...a,...Object.keys(g).length>0&&{designModeSettings:g}}},"persistCreationModePickerSettings",0,n])},285832,e=>{"use strict";var t=e.i(351623),r=e.i(361116),o=e.i(344480);e.i(975473);let i={},n=t.gql`
    fragment QuickDeployDialogHostingBuildArtifactFields on HostingBuildArtifact {
  id
  name
  type
  folderName
  services {
    id
    name
    paths
    hasRunCommand
  }
}
    `,l=t.gql`
    fragment QuickDeployDialogCurrentBuild2 on HostingBuild {
  id
  status
  timeCreated
  suspendedReason
  isPrivate
  hasPrivatePassword
  user {
    id
    displayName
  }
  repl {
    id
    hostingDeployment {
      ... on HostingDeployment {
        id
        agentInboxEnabled
        agentInboxConfig {
          position
          bgColor
          logoSrc
        }
      }
    }
  }
  artifacts {
    ...QuickDeployDialogHostingBuildArtifactFields
  }
  ...PublishDomainsRow
}
    ${n}
${r.PublishDomainsRowFragmentDoc}`,s=t.gql`
    query SuggestDeployAuthz($replId: String!, $provider: HostingBuildProvider!) {
  getRepl(id: $replId) {
    ... on Repl {
      id
      authorizations {
        editDeploymentSubdomain(provider: $provider) {
          isAuthorized
          message
        }
      }
    }
    ... on Error {
      message
    }
  }
}
    `;e.s(["QuickDeployDialogCurrentBuild2FragmentDoc",0,l,"useSuggestDeployAuthzQuery",0,function(e){let t={...i,...e};return o.useQuery(s,t)}])},903021,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment ArtifactUtilsHostingBuildArtifactFields on HostingBuildArtifact {
  type
  services {
    paths
    hasRunCommand
  }
}
    `;e.s(["ArtifactUtilsHostingBuildArtifactFieldsFragmentDoc",0,r])},361116,e=>{"use strict";var t=e.i(351623),r=e.i(903021);let o=t.gql`
    fragment PublishDomainsRow on HostingBuild {
  id
  provider
  artifacts {
    ...ArtifactUtilsHostingBuildArtifactFields
  }
  repl {
    id
    slug
    owner {
      ... on Team {
        id
        username
      }
      ... on User {
        id
        username
      }
    }
    hostingDeployment {
      ... on HostingDeployment {
        id
        replitAppSubdomain
        domains2 {
          id
          domain
          state
        }
      }
    }
  }
}
    ${r.ArtifactUtilsHostingBuildArtifactFieldsFragmentDoc}`;e.s(["PublishDomainsRowFragmentDoc",0,o])},780902,e=>{"use strict";var t=e.i(497644),r=e.i(68701);e.s(["useIsMobile",0,e=>{let o=(0,r.useUserAgent)();return(0,t.isMobile)({...e,ua:o??void 0})}])},218766,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment ReplAgentStatusLatestAgentStatus on AgentStatus {
  status
  statusV2
  label
  updatedAt
  appImageUrl
}
    `,o=t.gql`
    fragment ReplAgentStatusRepl on Repl {
  id
  latestAgentStatus {
    ...ReplAgentStatusLatestAgentStatus
  }
}
    ${r}`;e.s(["ReplAgentStatusReplFragmentDoc",0,o])},540742,e=>{"use strict";var t=e.i(15801),r=e.i(780902),o=e.i(753451);let i=new Set(["/replEnvironmentDesktop","/replEnvironmentMobile"]);e.s(["default",0,function(){let e=(0,o.isInBonsaiWebview)((0,t.useRouter)());return(0,r.useIsMobile)({tablet:!1})||e?"/replEnvironmentMobile":"/replEnvironmentDesktop"},"isWorkspacePathname",0,function(e){return i.has(e)}])},260666,e=>{"use strict";var t=e.i(351623),r=e.i(285832),o=e.i(344480);e.i(975473);let i={};r.QuickDeployDialogCurrentBuild2FragmentDoc;let n=t.gql`
    fragment DeployRepl on Repl {
  id
  hostingDeployment {
    ... on HostingDeployment {
      id
      replitAppSubdomain
      currentBuild {
        id
        status
        provider
      }
      inProgressBuild {
        id
        status
      }
    }
  }
}
    `,l=t.gql`
    query DeployPreloadedRepl($replId: String!) {
  getRepl(id: $replId) {
    ... on Repl {
      ...DeployRepl
    }
  }
}
    ${n}`;e.s(["DeployReplFragmentDoc",0,n,"useDeployPreloadedReplQuery",0,function(e){let t={...i,...e};return o.useQuery(l,t)}])},317349,e=>{"use strict";var t=e.i(351623),r=e.i(299020);let o={},i=t.gql`
    fragment DeleteReplDialogRepl on Repl {
  id
  title
}
    `,n=t.gql`
    mutation DeleteReplDialogReplDelete($id: String!) {
  deleteRepl(id: $id) {
    id
  }
}
    `;e.s(["DeleteReplDialogReplFragmentDoc",0,i,"useDeleteReplDialogReplDeleteMutation",0,function(e){let t={...o,...e};return r.useMutation(n,t)}])},748538,e=>{"use strict";var t=e.i(351623),r=e.i(299020);let o={},i=t.gql`
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
    `,n=t.gql`
    mutation EditReplFormEdit($input: UpdateReplInput!) {
  updateRepl(input: $input) {
    repl {
      id
      ...EditReplFormRepl
    }
  }
}
    ${i}`;e.s(["EditReplFormReplFragmentDoc",0,i,"useEditReplFormEditMutation",0,function(e){let t={...o,...e};return r.useMutation(n,t)}])},80593,e=>{"use strict";var t=e.i(351623),r=e.i(299020);let o={},i=t.gql`
    fragment LeaveMultiplayerReplDialogRepl on Repl {
  id
  title
}
    `,n=t.gql`
    mutation LeaveMultiplayerReplDialogRemove($id: String!) {
  removeSharedRepl(replId: $id) {
    id
  }
}
    `;e.s(["LeaveMultiplayerReplDialogReplFragmentDoc",0,i,"useLeaveMultiplayerReplDialogRemoveMutation",0,function(e){let t={...o,...e};return r.useMutation(n,t)}])},349565,e=>{"use strict";e.s(["ECONOMY_MODEL_BY_GQL",0,{CLAUDE_4_6_SONNET:"claude-4-6-sonnet",GPT_5_6_LUNA:"gpt-5-6-luna",GPT_5_6_LUNA_FAST:"gpt-5-6-luna-fast",CLAUDE_5_SONNET:"claude-5-sonnet",GPT_5_6_TERRA:"gpt-5-6-terra",GEMINI_3_1_PRO:"gemini-3-1-pro",GLM_5_2:"glm-5-2",ECONOMY_AUTO:"economy-auto",DESIGN_ECONOMY_AUTO:"design-economy-auto"},"ECONOMY_MODEL_SLUGS",0,["claude-4-6-sonnet","gpt-5-6-luna","gpt-5-6-luna-fast","claude-5-sonnet","gpt-5-6-terra","gemini-3-1-pro","glm-5-2","economy-auto","design-economy-auto"],"GQL_BY_MODEL_SLUG",0,{"free-auto":"FREE_AUTO","kimi-k2-7":"KIMI_K2_7","gpt-5-6-luna":"GPT_5_6_LUNA","gemini-3-5-flash":"GEMINI_3_5_FLASH","deepseek-v4-flash":"DEEPSEEK_V4_FLASH","claude-4-6-sonnet":"CLAUDE_4_6_SONNET","gpt-5-6-luna-fast":"GPT_5_6_LUNA_FAST","claude-5-sonnet":"CLAUDE_5_SONNET","gpt-5-6-terra":"GPT_5_6_TERRA","gemini-3-1-pro":"GEMINI_3_1_PRO","glm-5-2":"GLM_5_2","economy-auto":"ECONOMY_AUTO","design-economy-auto":"DESIGN_ECONOMY_AUTO","claude-fable-5":"CLAUDE_FABLE_5","claude-fable-5-1":"CLAUDE_FABLE_5_1","power-auto":"POWER_AUTO","intelligent-auto":"INTELLIGENT_AUTO","design-power-auto":"DESIGN_POWER_AUTO","claude-4-8-opus":"CLAUDE_4_8_OPUS","claude-4-8-opus-fast":"CLAUDE_4_8_OPUS_FAST","claude-5-opus":"CLAUDE_5_OPUS","claude-5-opus-fast":"CLAUDE_5_OPUS_FAST","kimi-k3":"KIMI_K3","gpt-5-6-terra-fast":"GPT_5_6_TERRA_FAST","gpt-5-6-sol":"GPT_5_6_SOL","gpt-6-astra":"GPT_6_ASTRA"},"LITE_MODEL_BY_GQL",0,{KIMI_K2_7:"kimi-k2-7",GPT_5_6_LUNA:"gpt-5-6-luna",GEMINI_3_5_FLASH:"gemini-3-5-flash",DEEPSEEK_V4_FLASH:"deepseek-v4-flash"},"LITE_MODEL_SLUGS",0,["kimi-k2-7","gpt-5-6-luna","gemini-3-5-flash","deepseek-v4-flash"],"MODEL_SLUG_BY_GQL",0,{FREE_AUTO:"free-auto",KIMI_K2_7:"kimi-k2-7",GPT_5_6_LUNA:"gpt-5-6-luna",GEMINI_3_5_FLASH:"gemini-3-5-flash",DEEPSEEK_V4_FLASH:"deepseek-v4-flash",CLAUDE_4_6_SONNET:"claude-4-6-sonnet",GPT_5_6_LUNA_FAST:"gpt-5-6-luna-fast",CLAUDE_5_SONNET:"claude-5-sonnet",GPT_5_6_TERRA:"gpt-5-6-terra",GEMINI_3_1_PRO:"gemini-3-1-pro",GLM_5_2:"glm-5-2",ECONOMY_AUTO:"economy-auto",DESIGN_ECONOMY_AUTO:"design-economy-auto",CLAUDE_FABLE_5:"claude-fable-5",CLAUDE_FABLE_5_1:"claude-fable-5-1",POWER_AUTO:"power-auto",INTELLIGENT_AUTO:"intelligent-auto",DESIGN_POWER_AUTO:"design-power-auto",CLAUDE_4_8_OPUS:"claude-4-8-opus",CLAUDE_4_8_OPUS_FAST:"claude-4-8-opus-fast",CLAUDE_5_OPUS:"claude-5-opus",CLAUDE_5_OPUS_FAST:"claude-5-opus-fast",KIMI_K3:"kimi-k3",GPT_5_6_TERRA_FAST:"gpt-5-6-terra-fast",GPT_5_6_SOL:"gpt-5-6-sol",GPT_6_ASTRA:"gpt-6-astra"},"POWER_MODEL_BY_GQL",0,{CLAUDE_FABLE_5:"claude-fable-5",CLAUDE_FABLE_5_1:"claude-fable-5-1",POWER_AUTO:"power-auto",INTELLIGENT_AUTO:"intelligent-auto",DESIGN_POWER_AUTO:"design-power-auto",CLAUDE_4_8_OPUS:"claude-4-8-opus",CLAUDE_4_8_OPUS_FAST:"claude-4-8-opus-fast",CLAUDE_5_OPUS:"claude-5-opus",CLAUDE_5_OPUS_FAST:"claude-5-opus-fast",KIMI_K3:"kimi-k3",GPT_5_6_TERRA_FAST:"gpt-5-6-terra-fast",GPT_5_6_SOL:"gpt-5-6-sol",GPT_6_ASTRA:"gpt-6-astra"},"POWER_MODEL_SLUGS",0,["claude-fable-5","claude-fable-5-1","power-auto","intelligent-auto","design-power-auto","claude-4-8-opus","claude-4-8-opus-fast","claude-5-opus","claude-5-opus-fast","kimi-k3","gpt-5-6-terra-fast","gpt-5-6-sol","gpt-6-astra"]])},615593,e=>{"use strict";var t=e.i(349565);function r(e){switch(e.modelProfile){case"FREE":return"free";case"LITE":return"lite";case"POWER":case"TURBO":return"power";case"ECONOMY":case"UNSPECIFIED":return"economy"}}let o=["lite","economy","power"];function i(e){return o.some(t=>void 0!==e[t])?o.filter(t=>!e[t]?.length):void 0}function n(e){switch(e){case"free":return"FREE";case"lite":return"LITE";case"power":return"POWER";case"economy":return"ECONOMY"}}function l(e){return"TURBO"===e.modelProfile}let s=[...t.LITE_MODEL_SLUGS,...t.ECONOMY_MODEL_SLUGS,...t.POWER_MODEL_SLUGS];e.s(["getAgentConfigDiffForEffortClear",0,function(e){let t=s.find(t=>t===e);return void 0===t?null:{modelEfforts:{[t]:void 0},enableHighEffort:!1}},"getAgentConfigDiffForEffortPick",0,function(e,t){let r=s.find(t=>t===e);return void 0===r?null:{modelEfforts:{[r]:t},enableHighEffort:!1}},"getAgentConfigDiffForModelClear",0,function(e){switch(e){case"lite":return{liteModel:void 0};case"power":return{powerModel:void 0};case"economy":return{economyModel:void 0};case"free":return{}}},"getAgentConfigDiffForModelPick",0,function(e,r){if("lite"===e){let e=t.LITE_MODEL_SLUGS.find(e=>e===r);return void 0!==e?{liteModel:e}:null}if("economy"===e){let e=t.ECONOMY_MODEL_SLUGS.find(e=>e===r);return void 0!==e?{economyModel:e}:null}if("power"===e){let e=t.POWER_MODEL_SLUGS.find(e=>e===r);return void 0!==e?{powerModel:e}:null}return null},"getAgentConfigDiffForUIMode",0,function(e,t=!1){let r=n(e),o=t?{enableTurbo:!1}:{};return"lite"===e?{modelProfile:r,mode:"BUILD",...o}:{modelProfile:r,...o}},"getModelProfileForUIMode",0,n,"getSelectedEffortForModel",0,function(e,t){let r=s.find(t=>t===e);return void 0===r?void 0:t.modelEfforts?.[r]},"getSelectedModelForUIMode",0,function(e,t){switch(e){case"power":return t.powerModel;case"lite":return t.liteModel;case"economy":return t.economyModel;case"free":return}},"getUIModeFromAgentConfig",0,r,"hasServedAgentModels",0,function(e){return Object.values(e).some(e=>void 0!==e&&e.length>0)},"isAgentConfigTriggerOrange",0,function(e,t){return!t&&(l(e)||!0===e.enableHighEffort)},"isSelectedAgentModeUnavailable",0,function(e,t){return i(t)?.includes(r(e))??!1},"isTurboEnabled",0,l,"unavailableAgentModes",0,i])},640326,e=>{"use strict";var t=e.i(351623),r=e.i(721037),o=e.i(344480),i=e.i(975473),n=e.i(299020);let l={},s=t.gql`
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
    ${r.UserReplSettingsFragmentFragmentDoc}`,a=t.gql`
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
    ${r.UserReplSettingsFragmentFragmentDoc}`;e.s(["useChateauAgentConfigPersistenceQuery",0,function(e){let t={...l,...e};return o.useQuery(s,t)},"useChateauAvailableAgentModelsRefreshLazyQuery",0,function(e){let t={...l,...e};return i.useLazyQuery(a,t)},"useUpdateChateauAgentConfigMutation",0,function(e){let t={...l,...e};return n.useMutation(u,t)}])},462669,e=>{"use strict";var t=e.i(389959),r=e.i(830675),o=e.i(908796),i=e.i(640326),n=e.i(11990),l=e.i(335421),s=e.i(211025),a=e.i(473072),u=e.i(29079),d=e.i(577671),p=e.i(46391),g=e.i(786864),c=e.i(349565),f=e.i(615593);let m={free:{},lite:c.LITE_MODEL_BY_GQL,economy:c.ECONOMY_MODEL_BY_GQL,power:c.POWER_MODEL_BY_GQL},E={[o.UserReplModelTier.Lite]:"lite",[o.UserReplModelTier.Economy]:"economy",[o.UserReplModelTier.Power]:"power",[o.UserReplModelTier.Free]:"free"},_={[o.AgentModelProvider.Anthropic]:"anthropic",[o.AgentModelProvider.Deepseek]:"deepseek",[o.AgentModelProvider.Google]:"google",[o.AgentModelProvider.Moonshot]:"moonshot",[o.AgentModelProvider.Openai]:"openai",[o.AgentModelProvider.Replit]:"replit",[o.AgentModelProvider.Zai]:"zai"},R={[o.UserReplAgentEffort.Low]:"low",[o.UserReplAgentEffort.Medium]:"medium",[o.UserReplAgentEffort.High]:"high",[o.UserReplAgentEffort.ExtraHigh]:"xhigh",[o.UserReplAgentEffort.Max]:"max"},S={low:o.UserReplAgentEffort.Low,medium:o.UserReplAgentEffort.Medium,high:o.UserReplAgentEffort.High,xhigh:o.UserReplAgentEffort.ExtraHigh,max:o.UserReplAgentEffort.Max},M={build:o.UserReplAgentMode.Build,design:o.UserReplAgentMode.Design};function U(e,t){let r=e.find(e=>e.mode===M[t]);return r?y(r.tiers):{}}function y(e){let t={};for(let r of e){let e=E[r.tier],o=m[e],i=r.models.flatMap(e=>{let t=o[e.model];return void 0!==t?[{slug:t,label:e.label,isDefault:e.isDefault,provider:_[e.provider],relativeCost:e.relativeCost,recommendedEffort:null==e.recommendedEffort?null:R[e.recommendedEffort],...e.authorization?.isAuthorized===!1?{authorization:e.authorization}:{}}]:[]});i.length>0&&(t[e]=i)}return t}function A(e){return e?.find(e=>e.isDefault)??e?.find(e=>l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e.slug))}function T(e){return A(e)??e?.[0]}function h(e,t){return t?e:Object.fromEntries(Object.entries(e).map(([e,t])=>{let r=(t??[]).filter(e=>!l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e.slug));return[e,t?.some(e=>e.isDefault&&l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e.slug))?function(e){if(e.some(e=>e.isDefault))return e;let t=e.find(e=>void 0===e.authorization);return void 0===t?e:e.map(e=>e===t?{...e,isDefault:!0}:e)}(r):r]}))}function L(e){return Object.fromEntries(Object.entries(e).map(([e,t])=>[e,t?.filter(e=>e.slug!==n.INTELLIGENT_AUTO_MODEL_SLUG)]))}function b(){let e=(0,a.default)(),[r]=(0,i.useChateauAvailableAgentModelsRefreshLazyQuery)({fetchPolicy:"network-only",errorPolicy:"none"});return(0,t.useCallback)(()=>r({variables:{replId:e}}),[r,e])}let O=["liteModel","economyModel","powerModel"];function v(e,t={}){if(!e)return{mode:"UNSPECIFIED",modelProfile:"UNSPECIFIED",enableTurbo:void 0,enableAutomatedTesting:void 0,enableHighEffort:void 0,liteModel:void 0,economyModel:void 0,powerModel:void 0,modelEfforts:void 0};let r=new Set(e.explicitFields),i=r.has(o.UserReplAgentSettingsField.IsTurboEnabled)&&null!=e.isTurboEnabled?e.isTurboEnabled:void 0,n=(()=>{if(!r.has(o.UserReplAgentSettingsField.ModelTier))return"UNSPECIFIED";switch(e.modelTier){case o.UserReplModelTier.Lite:return"LITE";case o.UserReplModelTier.Economy:return"ECONOMY";case o.UserReplModelTier.Power:return!0===i?"TURBO":"POWER";case o.UserReplModelTier.Free:return"FREE";default:return"UNSPECIFIED"}})(),l=r.has(o.UserReplAgentSettingsField.IsPlanModeEnabled)?!0===e.isPlanModeEnabled?"PLAN":"BUILD":"UNSPECIFIED",s=r.has(o.UserReplAgentSettingsField.IsAppTestingEnabled)&&null!=e.isAppTestingEnabled?e.isAppTestingEnabled:void 0,a=r.has(o.UserReplAgentSettingsField.IsHighEffortEnabled)&&null!=e.isHighEffortEnabled?e.isHighEffortEnabled:void 0,u=D(e),d=C(e),p=r.has(o.UserReplAgentSettingsField.LiteModel)&&null!=e.liteModel?P(c.LITE_MODEL_BY_GQL[e.liteModel],t.lite):void 0,g=r.has(o.UserReplAgentSettingsField.EconomyModel)&&null!=e.economyModel?P(c.ECONOMY_MODEL_BY_GQL[e.economyModel],t.economy):void 0;return{mode:l,modelProfile:n,enableTurbo:i,enableAutomatedTesting:s,enableHighEffort:a,intelligentAutoMode:u,autoPublish:d,liteModel:p,economyModel:g,powerModel:r.has(o.UserReplAgentSettingsField.PowerModel)&&null!=e.powerModel?P(c.POWER_MODEL_BY_GQL[e.powerModel],t.power):void 0,modelEfforts:(()=>{let t={};for(let r of e.modelEfforts){let e=c.MODEL_SLUG_BY_GQL[r.model];void 0!==e&&(t[e]=R[r.effort])}return Object.keys(t).length>0?t:void 0})()}}function D(e){if(e?.explicitFields.includes(o.UserReplAgentSettingsField.IsIntelligentAutoModeEnabled))return e.isIntelligentAutoModeEnabled??void 0}function C(e){if(e?.explicitFields.includes(o.UserReplAgentSettingsField.IsAutoPublishEnabled))return e.isAutoPublishEnabled??void 0}function P(e,t){return void 0===e||void 0===t||t.some(t=>t.slug===e&&void 0===t.authorization)?e:void 0}function I(e,t="build",r=!1,i=!0){if("design"===t){let t=function(e){let t={};if(void 0!==e.modelProfile)switch(e.modelProfile){case"LITE":t.modelTier=o.UserReplModelTier.Lite;break;case"ECONOMY":t.modelTier=o.UserReplModelTier.Economy;break;case"POWER":case"TURBO":t.modelTier=o.UserReplModelTier.Power;break;case"FREE":t.modelTier=o.UserReplModelTier.Free}return void 0!==e.enableAutomatedTesting&&(t.isAppTestingEnabled=e.enableAutomatedTesting),F(e,t),Object.keys(t).length>0?{designModeSettings:t}:{}}(e);return void 0!==e.intelligentAutoMode&&(t.isIntelligentAutoModeEnabled=e.intelligentAutoMode),void 0!==e.autoPublish&&(t.isAutoPublishEnabled=e.autoPublish),{agentSettings:t,taskSettings:{}}}let n={},l={};if(void 0!==e.modelProfile)switch(e.modelProfile){case"LITE":n.modelTier=r?null:o.UserReplModelTier.Lite,n.isPlanModeEnabled=!1,!1===i&&(l.isAutoApprovePlanEnabled=!1);break;case"ECONOMY":n.modelTier=o.UserReplModelTier.Economy;break;case"POWER":case"TURBO":n.modelTier=o.UserReplModelTier.Power;break;case"FREE":n.modelTier=o.UserReplModelTier.Free,!1===i&&(l.isAutoApprovePlanEnabled=!1)}return void 0!==e.mode&&("PLAN"===e.mode?n.isPlanModeEnabled=!0:"BUILD"===e.mode&&(n.isPlanModeEnabled=!1)),void 0!==e.enableAutomatedTesting&&(n.isAppTestingEnabled=e.enableAutomatedTesting),void 0!==e.enableTurbo&&(n.isTurboEnabled=e.enableTurbo),void 0!==e.enableHighEffort&&(n.isHighEffortEnabled=e.enableHighEffort),void 0!==e.intelligentAutoMode&&(n.isIntelligentAutoModeEnabled=e.intelligentAutoMode),void 0!==e.autoPublish&&(n.isAutoPublishEnabled=e.autoPublish),F(e,n),{agentSettings:n,taskSettings:l}}function F(e,t){if("liteModel"in e&&(t.liteModel=void 0===e.liteModel?null:c.GQL_BY_MODEL_SLUG[e.liteModel]),"economyModel"in e&&(t.economyModel=void 0===e.economyModel?null:c.GQL_BY_MODEL_SLUG[e.economyModel]),"powerModel"in e&&(t.powerModel=void 0===e.powerModel?null:c.GQL_BY_MODEL_SLUG[e.powerModel]),void 0!==e.modelEfforts){let r=Object.entries(e.modelEfforts);r.length>0&&(t.modelEfforts=r.map(([e,t])=>({model:c.GQL_BY_MODEL_SLUG[e],effort:void 0===t?null:S[t]})))}}e.s(["availableAgentModeOptions",0,function({options:e,canUpgradeToTurbo:t,isPerTierAutoModeAvailable:r}){let o=L(h(e,r));return t?o:Object.fromEntries(Object.entries(o).map(([e,t])=>[e,t?.filter(e=>void 0===e.authorization)]))},"filterAutoModeModelOptions",0,h,"getDefaultModelOption",0,T,"getModelDefaultsForSend",0,function(e,t={}){let{isEffortCapped:r=!1,isPerTierAutoModeAvailable:o=!0}=t,i=h(e,o),a={};for(let e of["lite","economy","power"]){let t=A(i[e])?.slug;void 0!==t&&(a[e]=t)}let u={},d={};if(o)for(let e of["economy","power"]){let t=a[e];void 0!==t&&l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(t)&&(d[e]=t)}else for(let t of["lite","economy","power"]){let r=a[t];e[t]?.some(e=>e.isDefault&&l.USER_REPL_TIER_AUTO_MODEL_SLUGS.has(e.slug))&&void 0!==r&&(d[t]=r)}void 0!==a.power&&i.power?.every(e=>e.slug!==n.FABLE_5_MODEL_SLUG)&&(u.power=a.power);let p={};if(r)for(let e of Object.values(i))for(let t of e??[]){if(null===t.recommendedEffort)continue;let e=(0,s.clampToLadder)(t.recommendedEffort,s.CAPPED_EFFORT_LEVELS);e!==t.recommendedEffort&&(p[t.slug]=e)}return{defaultByTier:a,compatibilityPinByTier:u,...Object.keys(d).length>0&&{pinnedDefaultByTier:d},...Object.keys(p).length>0&&{cappedEffortByModel:p}}},"mapAvailableAgentModelsToOptions",0,y,"mapChateauConfigDiffToUserSettingsInput",0,I,"modeModelOptionsFor",0,function(e,t){let r={};for(let o of Object.values(E)){let i=e[o];if(void 0===i||0===i.length)continue;let n=(0,f.getSelectedModelForUIMode)(o,t);r[o]=i.find(e=>e.slug===n)??T(i)}return r},"modelOptionsForMode",0,U,"useAvailableAgentModels",0,function(e="build"){let r=(0,a.default)(),{isComplete:o}=(0,p.useUserReplSettingsBootstrapState)(),{data:n}=(0,i.useChateauAgentConfigPersistenceQuery)({variables:{replId:r},skip:!o}),l=n?.getRepl?.__typename==="Repl"?n.getRepl.currentUserSettings?.visibleAgentModelsByMode:void 0;return(0,t.useMemo)(()=>l?U(l,e):{},[l,e])},"useChateauAgentSettingsPersistence",0,function(e="build"){let n=(0,a.default)(),{isComplete:l}=(0,p.useUserReplSettingsBootstrapState)(),{shouldSeeFreemiumExperience:s}=(0,g.useShouldSeeFreemiumExperience)(n),{freeLiteTaskAutoApproval:f,loading:m}=(0,u.useAgentAuthorization)(),E=m||f?.isAuthorized!==!1,{data:_,loading:S,error:M}=(0,i.useChateauAgentConfigPersistenceQuery)({variables:{replId:n},skip:!l}),y=b(),A=(0,t.useRef)(Promise.resolve()),T=(0,t.useRef)(!1);(0,t.useEffect)(()=>{M&&!T.current&&(T.current=!0,r.captureException(M,{level:"warning",tags:{component:"useChateauAgentSettingsPersistence"},extra:{message:"falling back to default chateau agent config",replId:n}}))},[M,n]);let[h]=(0,i.useUpdateChateauAgentConfigMutation)(),L=_?.getRepl?.__typename==="Repl"?_.getRepl.currentUserSettings?.agentSettings??null:null,F=_?.getRepl?.__typename==="Repl"?_.getRepl.currentUserSettings?.visibleAgentModelsByMode:void 0,w=(0,t.useMemo)(()=>F?U(F,e):{},[F,e]),k=l&&(void 0!==_||void 0!==M);return{initialConfig:(0,t.useMemo)(()=>{if(k)return"design"===e?function(e,t={},r=null){let i={mode:"UNSPECIFIED",modelProfile:"UNSPECIFIED",enableTurbo:void 0,enableAutomatedTesting:void 0,enableHighEffort:void 0,intelligentAutoMode:D(r),autoPublish:C(r),liteModel:void 0,economyModel:void 0,powerModel:void 0,modelEfforts:void 0};if(!e)return i;let n=new Set(e.explicitFields),l=(()=>{if(!n.has(o.UserReplModeAgentSettingsField.ModelTier))return"UNSPECIFIED";switch(e.modelTier){case o.UserReplModelTier.Lite:return"LITE";case o.UserReplModelTier.Economy:return"ECONOMY";case o.UserReplModelTier.Power:return"POWER";case o.UserReplModelTier.Free:return"FREE";default:return"UNSPECIFIED"}})(),s=n.has(o.UserReplModeAgentSettingsField.IsAppTestingEnabled)&&null!=e.isAppTestingEnabled?e.isAppTestingEnabled:void 0,a=n.has(o.UserReplModeAgentSettingsField.LiteModel)&&null!=e.liteModel?P(c.LITE_MODEL_BY_GQL[e.liteModel],t.lite):void 0,u=n.has(o.UserReplModeAgentSettingsField.EconomyModel)&&null!=e.economyModel?P(c.ECONOMY_MODEL_BY_GQL[e.economyModel],t.economy):void 0,d=n.has(o.UserReplModeAgentSettingsField.PowerModel)&&null!=e.powerModel?P(c.POWER_MODEL_BY_GQL[e.powerModel],t.power):void 0,p=(()=>{let t={};for(let r of e.modelEfforts){let e=c.MODEL_SLUG_BY_GQL[r.model];void 0!==e&&(t[e]=R[r.effort])}return Object.keys(t).length>0?t:void 0})();return{...i,modelProfile:l,enableAutomatedTesting:s,liteModel:a,economyModel:u,powerModel:d,modelEfforts:p}}(L?.designModeSettings??null,w,L):v(L,w)},[k,e,w,L]),persistConfig:(0,t.useCallback)(async t=>{var o;let i=I(t,e,s,E);if(Object.values((o=i).agentSettings).some(e=>void 0!==e)||Object.values(o.taskSettings).some(e=>void 0!==e))try{let e=await h({variables:{input:{replId:n,agentSettings:i.agentSettings,taskSettings:i.taskSettings}},update:(e,t)=>{let r=t.data?.updateCurrentUserReplSettings;r?.__typename==="UpdateCurrentUserReplSettingsPayload"&&(0,d.writeCurrentUserReplSettingsToCache)({cache:e,replId:n,settings:r.userReplSettings})}}),o=e.data?.updateCurrentUserReplSettings;if(o?.__typename!=="UpdateCurrentUserReplSettingsPayload")return void r.captureMessage("updateCurrentUserReplSettings returned non-payload",{extra:{replId:n,typename:o?.__typename,message:o&&"message"in o?o.message:void 0}});O.some(e=>e in t&&void 0===t[e])&&(A.current=A.current.then(y).catch(()=>void 0))}catch(e){r.captureException(e,{extra:{message:"Error persisting chateau agent config",replId:n,configDiff:t}})}},[h,n,y,e,s,E]),loading:!l||S||m}},"useRawBuildAgentConfig",0,function(){let e=(0,a.default)(),{isComplete:r}=(0,p.useUserReplSettingsBootstrapState)(),{data:o}=(0,i.useChateauAgentConfigPersistenceQuery)({variables:{replId:e},skip:!r}),n=o?.getRepl?.__typename==="Repl"?o.getRepl.currentUserSettings:void 0;return(0,t.useMemo)(()=>v(n?.agentSettings??null,n?.visibleAgentModelsByMode?U(n.visibleAgentModelsByMode,"build"):{}),[n])},"useRefreshAvailableAgentModels",0,b,"withoutIntelligentAutoOption",0,L])},721037,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var o=e.i(299020);let i={},n=t.gql`
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
    ${n}`,s=t.gql`
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
    ${n}`,a=t.gql`
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
    ${n}`;e.s(["CurrentUserReplSettingsCacheWriteFragmentDoc",0,l,"UserReplSettingsFragmentFragmentDoc",0,n,"useCurrentUserReplSettingsQuery",0,function(e){let t={...i,...e};return r.useQuery(s,t)},"useUpdateCurrentUserReplSettingsMutation",0,function(e){let t={...i,...e};return o.useMutation(a,t)}])},577671,e=>{"use strict";var t=e.i(389959),r=e.i(830675),o=e.i(721037);function i({cache:e,replId:t,settings:r}){let n=e.identify({__typename:"Repl",id:t});e.writeFragment({id:n,fragment:o.CurrentUserReplSettingsCacheWriteFragmentDoc,fragmentName:"CurrentUserReplSettingsCacheWrite",data:{__typename:"Repl",id:t,currentUserSettings:r}}),e.modify({id:n,fields:{currentUserSettings:e=>({...e,...r})}})}e.s(["useCurrentUserReplSettings",0,function(e){let{data:n,loading:l}=(0,o.useCurrentUserReplSettingsQuery)({variables:{replId:e}}),[s]=(0,o.useUpdateCurrentUserReplSettingsMutation)(),a=n?.getRepl?.__typename==="Repl"?n.getRepl:null,u=a?.currentUserSettings?.agentSettings??null,d=a?.currentUserSettings?.taskSettings??null;return{isResolved:null!==a,loading:l,agentSettings:u,taskSettings:d,updateSettings:(0,t.useCallback)(async t=>{try{let o=await s({variables:{input:{replId:e,...t}},update:(t,r)=>{let o=r.data?.updateCurrentUserReplSettings;o?.__typename==="UpdateCurrentUserReplSettingsPayload"&&i({cache:t,replId:e,settings:o.userReplSettings})}}),n=o.data?.updateCurrentUserReplSettings;if(n?.__typename==="UpdateCurrentUserReplSettingsPayload")return!0;return r.captureMessage("updateCurrentUserReplSettings returned non-payload",{extra:{replId:e,typename:n?.__typename,message:n&&"message"in n?n.message:void 0}}),!1}catch(o){return r.captureException(o,{extra:{message:"Error updating current user repl settings",replId:e,input:t}}),!1}},[e,s])}},"writeCurrentUserReplSettingsToCache",0,i])},925712,e=>{"use strict";var t=e.i(351623),r=e.i(721037),o=e.i(344480);e.i(975473);var i=e.i(299020);let n={},l=t.gql`
    fragment UserReplSettingsBootstrapFragment on UserReplSettings {
  isBootstrapped
  ...UserReplSettingsFragment
}
    ${r.UserReplSettingsFragmentFragmentDoc}`,s=t.gql`
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
    ${l}`;e.s(["useBootstrapCurrentUserReplSettingsMutation",0,function(e){let t={...n,...e};return i.useMutation(a,t)},"useUserReplSettingsBootstrapQuery",0,function(e){let t={...n,...e};return o.useQuery(s,t)}])},567314,e=>{"use strict";var t=e.i(389959),r=e.i(830675),o=e.i(925712),i=e.i(577671);let n=[250,1e3];async function l({attempt:e,bootstrap:t,replId:o}){try{let e=await t({variables:{input:{replId:o}}}),i=e.data?.bootstrapCurrentUserReplSettings;if(i?.__typename==="BootstrapCurrentUserReplSettingsPayload")return;r.captureMessage("bootstrapCurrentUserReplSettings returned non-payload",{extra:{replId:o,typename:i?.__typename,message:i&&"message"in i?i.message:void 0}})}catch(s){let r=n[e];if(null!=r){var i;return await (i=r,new Promise(e=>{setTimeout(e,i)})),l({attempt:e+1,bootstrap:t,replId:o})}throw s}}e.s(["useUserReplSettingsBootstrap",0,function(e,n){let s=(0,t.useRef)(new Set),[a,u]=(0,t.useState)(()=>({isComplete:!1,isBootstrapping:!1})),{data:d,loading:p}=(0,o.useUserReplSettingsBootstrapQuery)({variables:{replId:n??""},skip:!e||null===n}),[g]=(0,o.useBootstrapCurrentUserReplSettingsMutation)({update(e,t){let r=t.data?.bootstrapCurrentUserReplSettings;r?.__typename!=="BootstrapCurrentUserReplSettingsPayload"||null!==n&&(0,i.writeCurrentUserReplSettingsToCache)({cache:e,replId:n,settings:r.userReplSettings})}}),c=d?.getRepl.__typename==="Repl"?d.getRepl:null,f=d?.currentUser!=null,m=null!=c,E=c?.currentUserSettings?.isBootstrapped===!0;return(0,t.useEffect)(()=>{if(p)return void u({isComplete:!1,isBootstrapping:!1});if(null===n||!f||!m||E)return void u({isComplete:!0,isBootstrapping:!1});if(s.current.has(n))return;s.current.add(n);let e=!1;return u({isComplete:!1,isBootstrapping:!0}),(async()=>{try{await l({attempt:0,bootstrap:g,replId:n})}catch(e){r.captureException(e,{extra:{message:"Error bootstrapping current user repl settings",replId:n}})}finally{e||u({isComplete:!0,isBootstrapping:!1})}})(),()=>{e=!0}},[g,f,m,E,p,n]),a}])},46391,e=>{"use strict";var t=e.i(276385),r=e.i(389959),o=e.i(567314);let i=(0,r.createContext)({isComplete:!0,isBootstrapping:!1});e.s(["UserReplSettingsBootstrapProvider",0,function({children:e,enabled:n=!0,replId:l}){let s=(0,o.useUserReplSettingsBootstrap)(n,l),a=(0,r.useMemo)(()=>({isComplete:s.isComplete,isBootstrapping:s.isBootstrapping}),[s.isComplete,s.isBootstrapping]);return(0,t.jsx)(i.Provider,{value:a,children:e})},"useUserReplSettingsBootstrapState",0,function(){return(0,r.useContext)(i)}])},301128,e=>{"use strict";var t=e.i(960933);let r=t.Type.Union([t.Type.Object({type:t.Type.Literal("recent")},{additionalProperties:!1}),t.Type.Object({type:t.Type.Literal("pinned")},{additionalProperties:!1}),t.Type.Object({type:t.Type.Literal("custom"),sectionId:t.Type.String({pattern:"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"})},{additionalProperties:!1})]);e.s(["SidebarDestinationSchema",0,r])},306229,e=>{"use strict";var t=e.i(960933),r=e.i(862927),o=e.i(598986),i=e.i(301128);let n="replit:home-sidebar-destination",l=t.Type.Object({intentId:t.Type.String(),userId:t.Type.Number(),orgId:t.Type.Union([t.Type.String(),t.Type.Null()]),destination:i.SidebarDestinationSchema,createdAt:t.Type.Number()});e.s(["clearHomeSidebarDestination",0,function(){o.default.remove(n)},"consumeHomeSidebarDestination",0,function(e){let t=o.default.get(n);void 0!==e&&r.Value.Check(l,t)&&t.intentId===e&&o.default.remove(n)},"readHomeSidebarDestination",0,function(e,t){let i=o.default.get(n);if(!(!r.Value.Check(l,i)||i.userId!==e||i.orgId!==(t??null)||Date.now()-i.createdAt>18e5))return{intentId:i.intentId,destination:i.destination}},"setHomeSidebarDestination",0,function(e,t,r){o.default.set(n,{intentId:crypto.randomUUID(),userId:e,orgId:t??null,destination:r,createdAt:Date.now()})}])},11990,e=>{"use strict";e.s(["FABLE_5_MODEL_SLUG",0,"claude-fable-5","INTELLIGENT_AUTO_MODEL_SLUG",0,"intelligent-auto","TURBO_CARRYOVER_MODEL_SLUG",0,"claude-5-opus-fast"])},335421,e=>{"use strict";let t=["free-auto"],r=["kimi-k2-7","gpt-5-6-luna","gemini-3-5-flash","deepseek-v4-flash"],o=["claude-4-6-sonnet","gpt-5-6-luna","gpt-5-6-luna-fast","claude-5-sonnet","gpt-5-6-terra","gemini-3-1-pro","glm-5-2","economy-auto","design-economy-auto"],i=["claude-fable-5","claude-fable-5-1","power-auto","intelligent-auto","design-power-auto","claude-4-8-opus","claude-4-8-opus-fast","claude-5-opus","claude-5-opus-fast","kimi-k3","gpt-5-6-terra-fast","gpt-5-6-sol","gpt-6-astra"],n={free:t[0],lite:r[0],economy:o[0],power:i[0]},l=new Set(["free-auto","economy-auto","design-economy-auto","power-auto","intelligent-auto","design-power-auto"]),s=new Set(["free-auto","economy-auto","design-economy-auto","power-auto","design-power-auto"]);e.s(["USER_REPL_DEFAULT_MODELS_BY_TIER",0,n,"USER_REPL_ECONOMY_MODELS",0,o,"USER_REPL_EFFORT_LOCKED_SLUGS",0,l,"USER_REPL_FAST_MODE_MODEL_SLUGS",0,["claude-4-8-opus-fast","claude-5-opus-fast"],"USER_REPL_FREE_MODELS",0,t,"USER_REPL_LITE_MODELS",0,r,"USER_REPL_MODEL_BADGES",0,{},"USER_REPL_MODEL_LABELS",0,{"free-auto":"Auto","kimi-k2-7":"Kimi K2.7","gpt-5-6-luna":"GPT-5.6 Luna","gemini-3-5-flash":"Gemini 3.5 Flash","deepseek-v4-flash":"DeepSeek V4 Flash","claude-4-6-sonnet":"Claude Sonnet 4.6","gpt-5-6-luna-fast":"GPT-5.6 Luna Fast","claude-5-sonnet":"Claude Sonnet 5","gpt-5-6-terra":"GPT-5.6 Terra","gemini-3-1-pro":"Gemini 3.1 Pro","glm-5-2":"GLM 5.2","economy-auto":"Auto","design-economy-auto":"Auto","claude-fable-5":"Claude Fable 5","claude-fable-5-1":"Claude Fable 5.1","power-auto":"Auto","intelligent-auto":"Intelligence","design-power-auto":"Auto","claude-4-8-opus":"Claude Opus 4.8","claude-4-8-opus-fast":"Claude Opus 4.8 Fast","claude-5-opus":"Claude Opus 5","claude-5-opus-fast":"Claude Opus 5 Fast","kimi-k3":"Kimi K3","gpt-5-6-terra-fast":"GPT-5.6 Terra Fast","gpt-5-6-sol":"GPT-5.6 Sol","gpt-6-astra":"GPT-6 Astra"},"USER_REPL_MODEL_PROVIDERS",0,{"free-auto":"replit","kimi-k2-7":"moonshot","gpt-5-6-luna":"openai","gemini-3-5-flash":"google","deepseek-v4-flash":"deepseek","claude-4-6-sonnet":"anthropic","gpt-5-6-luna-fast":"openai","claude-5-sonnet":"anthropic","gpt-5-6-terra":"openai","gemini-3-1-pro":"google","glm-5-2":"zai","economy-auto":"replit","design-economy-auto":"replit","claude-fable-5":"anthropic","claude-fable-5-1":"anthropic","power-auto":"replit","intelligent-auto":"replit","design-power-auto":"replit","claude-4-8-opus":"anthropic","claude-4-8-opus-fast":"anthropic","claude-5-opus":"anthropic","claude-5-opus-fast":"anthropic","kimi-k3":"moonshot","gpt-5-6-terra-fast":"openai","gpt-5-6-sol":"openai","gpt-6-astra":"openai"},"USER_REPL_POWER_MODELS",0,i,"USER_REPL_TIER_AUTO_MODEL_SLUGS",0,s])}]);

//# debugId=1870298d-9512-80bc-cebe-49016a2aacba
//# sourceMappingURL=37vinssl63jn_.js.map