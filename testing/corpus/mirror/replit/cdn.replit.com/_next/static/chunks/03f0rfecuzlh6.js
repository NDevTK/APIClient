;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="a238420b-8c46-4257-d02d-1fe5ed3cd94e")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,341732,e=>{"use strict";var t=e.i(389959),n=e.i(62624),s=e.i(796424),i=e.i(436298);e.s(["useSelectableArtifactKinds",0,function(e=!0){let r=null!==(0,t.useContext)(s.default),a=(0,t.useMemo)(()=>(0,i.buildSelectableArtifactKinds)({isZealot:r}),[r]),o=(0,n.useMobileOutputOptions)(a);return e?o:a}])},668721,e=>{"use strict";var t=e.i(351623);let n=t.gql`
    fragment DomainListCurrentBuild2 on HostingBuild {
  id
  provider
  repl {
    id
    slug
    domains {
      ... on Domain {
        id
        hosting_deployment_id
        domain
        state
      }
    }
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
      }
    }
    rayfin {
      ... on ReplRayfin {
        workspaceId
        itemId
        tenantId
      }
    }
  }
}
    `;e.s(["DomainListCurrentBuild2FragmentDoc",0,n])},323604,e=>{"use strict";var t=e.i(351623);let n=t.gql`
    fragment DeploymentLink on HostingDeployment {
  id
  replitAppSubdomain
  deploymentBaseDomain
  domains2 {
    id
    domain
    state
  }
  currentBuild {
    id
    provider
    repl {
      id
      rayfin {
        ... on ReplRayfin {
          workspaceId
          itemId
          tenantId
        }
      }
    }
  }
}
    `;e.s(["DeploymentLinkFragmentDoc",0,n])},676107,e=>{"use strict";var t=e.i(351623),n=e.i(668721),s=e.i(323604),i=e.i(319801);let r=t.gql`
    fragment ReplDomain2 on Domain {
  id
  hosting_deployment_id
  domain
  state
}
    `,a=t.gql`
    fragment CustomDomain on Domain {
  ...ReplDomain2
}
    ${r}`,o=t.gql`
    fragment TargetHostingDeployment on HostingDeployment {
  id
  replitAppSubdomain
  timeCreated
  securityScanEnabled
  uptimeCheckEnabled
  agentInboxEnabled
  agentInboxConfig {
    position
    logoSrc
    bgColor
  }
  analyticsEnabled
  replitBadgeEnabled
  scheduledDeletionTime
  latestBuildStatus
  geography
}
    `,l=t.gql`
    fragment MachineConfiguration on HostingMachineConfiguration {
  id
  label
  vcpu
  memory
  slug
}
    `,u=t.gql`
    fragment HostingBuildArtifactFields on HostingBuildArtifact {
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
    `,d=t.gql`
    fragment CurrentBuild2 on HostingBuild {
  ...DomainListCurrentBuild2
  id
  description
  status
  hasDeployLogs
  suspendedReason
  timeCreated
  hasImageTag
  envVars {
    name
    value
  }
  repl {
    id
    slug
    apexProxy
    domains {
      ... on Domain {
        ...CustomDomain
      }
    }
    org {
      id
    }
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
        ...TargetHostingDeployment
        ...DeploymentLink
      }
    }
  }
  provider
  machineConfiguration {
    ...MachineConfiguration
  }
  maxMachineInstances
  machineJob {
    timezone
    crontab
  }
  user {
    id
    displayName
    username
    image
    fullName
  }
  isPrivate
  hasPrivatePassword
  isStandby
  artifacts {
    ...HostingBuildArtifactFields
  }
}
    ${n.DomainListCurrentBuild2FragmentDoc}
${a}
${o}
${s.DeploymentLinkFragmentDoc}
${l}
${u}`,c=t.gql`
    fragment DeploymentStatus on HostingDeployment {
  id
  currentBuild {
    id
    status
    suspendedReason
    timeCreated
    user {
      id
      displayName
    }
    provider
  }
  inProgressBuild {
    id
  }
  latestBuildStatus
}
    `,p=t.gql`
    fragment DeploymentItem on HostingDeployment {
  id
  replitAppSubdomain
  domains2 {
    id
    domain
    state
  }
  repl {
    id
    title
    iconUrl
    config {
      isAgentStack
    }
    ...ReplLinkRepl
  }
  ...DeploymentLink
  ...DeploymentStatus
}
    ${i.ReplLinkReplFragmentDoc}
${s.DeploymentLinkFragmentDoc}
${c}`;e.s(["CurrentBuild2FragmentDoc",0,d,"DeploymentItemFragmentDoc",0,p,"HostingBuildArtifactFieldsFragmentDoc",0,u,"MachineConfigurationFragmentDoc",0,l,"TargetHostingDeploymentFragmentDoc",0,o])},631567,e=>{"use strict";var t=e.i(351623),n=e.i(344480);e.i(975473);let s={},i=t.gql`
    query CurrentUserId {
  currentUser {
    id
  }
}
    `;e.s(["useCurrentUserIdQuery",0,function(e){let t={...s,...e};return n.useQuery(i,t)}])},570438,e=>{"use strict";var t=e.i(631567);e.s(["useCurrentUserId",0,function({skip:e}={}){let{data:n}=(0,t.useCurrentUserIdQuery)({skip:e});return n?.currentUser?.id??null},"useCurrentUserIdState",0,function({skip:e}={}){let{data:n,loading:s,error:i}=(0,t.useCurrentUserIdQuery)({skip:e});return{currentUserId:n?.currentUser?.id??null,isCurrentUserIdLoading:s,isCurrentUserResolved:!s&&!i&&void 0!==n}}])},62624,e=>{"use strict";var t=e.i(389959),n=e.i(436298),s=e.i(753451);e.s(["useMobileOutputOptions",0,function(e){let i=(0,s.useDoesBonsaiSupportFeature)("disableMobileAppCreationOniOS"),r="ios"===(0,s.useMobileAppPlatform)(),a=i&&r;return(0,t.useMemo)(()=>(0,n.getMobileOutputOptions)({excludeMobileArtifact:a,kinds:e}),[a,e])}])},725169,e=>{"use strict";var t=e.i(217403);e.s(["useObservableAtom",0,function(e,n){return(0,t.default)(()=>{var t,s;return t=n,s=e,{get current(){return t.get(s)},subscribe:e=>t.sub(s,()=>e(t.get(s)))}},[n,e])}])},892158,e=>{"use strict";var t=e.i(276385),n=e.i(36454),s=e.i(389959),i=e.i(405411),r=e.i(983420),a=e.i(152651),o=e.i(210853),l=e.i(406664),u=e.i(158627),d=e.i(27923),c=e.i(488299);let p=e.i(61732).SpecializedView.a,[m,h]=(0,d.twVar)({variable:"--icon-button-size"}),f=(0,s.forwardRef)(function(e,s){let{props:i,className:f,styles:g,attributes:y}=(0,u.useRuiComponentProps)("IconButtonLink",e),{alt:b,children:v,colorway:S,disabled:C,size:k=c.defaultIconSize,as:x,href:I,prefetch:w,replace:R,scroll:A,shallow:M,dataCy:D,variant:T,filled:_,onClick:$,onAuxClick:j,...K}=i,E=(0,o.useClickIntentHandlers)({onClick:$,onAuxClick:j}),B=(0,l.useCreateInteractive)({variant:T??(_?"filled":"nofill"),colorway:S,disabled:C}),L={...y,disabled:C,style:{...h(`${k}px`),...B.style,...g},className:d.tw.merge("flex justify-center items-center",d.tw.var(m("w-(--icon-button-size) h-(--icon-button-size)")),d.tw.external(B.clsx),d.tw.external(f)),ref:s,...K},O=(0,t.jsx)(r.IconProvider,{alt:b,size:c.IconSizeMap[k],children:v});return C?(0,t.jsx)(p,{"data-cy":D,"aria-disabled":C,role:"link",...L,children:O}):(0,t.jsx)(n.default,{"data-cy":D,role:"link",...a.INSTRUMENTED_MARKER_PROP,as:x,href:I,prefetch:w,replace:R,scroll:A,shallow:M,...L,...E,children:O})}),g=(0,s.forwardRef)(function(e,n){let{alt:s,tooltipBehavior:r,tooltipPlacement:a,tooltipContents:o,...l}=e;return(0,t.jsxs)(i.TooltipTrigger,{isDisabled:"hidden"===r,children:[(0,t.jsx)(f,{alt:s,ref:n,...l}),(0,t.jsx)(c.IconButtonTooltip,{placement:a,children:o??s})]})});e.s(["IconButtonLink",0,g])},114953,e=>{"use strict";e.i(242933);let t=new(e.i(790164)).ObservableState(null),n=0;e.s(["clearReplMainAgentTarget",0,function(e){t.current?.kind==="target"&&t.current.target.replId===e&&t.set(null)},"invalidateReplMainAgentTarget",0,function(e){n+=1,t.set({kind:"evicted",replId:e,version:n})},"replMainAgentTargetState",0,t,"resolveReplMainAgentInvalidation",0,function(e,n){t.current?.kind==="evicted"&&t.current.replId===e&&t.current.version===n&&t.set(null)},"setReplMainAgentTarget",0,function(e){let n=t.current;(n?.kind!=="target"||n.target.replId!==e.replId||n.target.agentId!==e.agentId||n.target.endpoint!==e.endpoint)&&t.set({kind:"target",target:e})}])},124575,e=>{e.v({submenuContainer:"NewArtifactSubmenu-module__2jvkpW__submenuContainer"})},280810,e=>{"use strict";var t=e.i(276385),n=e.i(625251),s=e.i(927600),i=e.i(109591),r=e.i(436298),a=e.i(341732);e.i(214847);var o=e.i(864300),l=e.i(919073),u=e.i(295231),d=e.i(773222),c=e.i(124575);function p({kind:e,onSelect:n}){let s=(0,o.useIntl)(),i=(0,r.getOutputKindConfig)(e),a=(0,r.getOutputKindLabel)(s,e);return(0,t.jsx)(u.MenuItem,{label:a,icon:(0,t.jsx)(i.Icon,{}),onAction:()=>n(e),dataCy:`artifact-picker-pill-${i.label.toLowerCase().replace(/\s+/g,"-")}`})}function m({onSelect:e,kinds:n,footer:s}){let i=(0,a.useSelectableArtifactKinds)();return(0,t.jsxs)(t.Fragment,{children:[(n??i).map(n=>(0,t.jsx)(p,{kind:n,onSelect:e},(0,r.outputKindKey)(n))),s]})}e.s(["NewArtifactPickerMenuItems",0,m,"NewArtifactSubmenu",0,function({onSelect:e,kinds:r,footer:a,onOpen:p,placement:h}){let f=(0,o.useIntl)();return(0,t.jsxs)(n.SubmenuTrigger,{delay:0,children:[(0,t.jsx)(u.MenuItem,{label:f.formatMessage({id:"workspace.newArtifactSubmenuTriggerLabel",defaultMessage:"Create something new"}),icon:(0,t.jsx)(i.default,{}),iconRight:(0,t.jsx)(s.default,{}),onPressStart:()=>p?.()}),(0,t.jsx)(d.RawPopover,{offset:4,placement:h,children:(0,t.jsx)(l.ShadesSurface,{colorShade:"themePopup",p:4,br:"container",clsx:c.default.submenuContainer,children:(0,t.jsx)(u.Menu,{"aria-label":f.formatMessage({id:"workspace.newArtifactSubmenuAriaLabel",defaultMessage:"Create something new"}),children:(0,t.jsx)(m,{onSelect:e,kinds:r,footer:a})})})})]})}])},465584,e=>{e.v({blockBorder:"KeyComboBlocks-module__3yf3FW__blockBorder",blockRoot:"KeyComboBlocks-module__3yf3FW__blockRoot",blockText:"KeyComboBlocks-module__3yf3FW__blockText",chord:"KeyComboBlocks-module__3yf3FW__chord",shortcut:"KeyComboBlocks-module__3yf3FW__shortcut"})},559357,e=>{"use strict";var t=e.i(276385),n=e.i(68701),s=e.i(295621),i=e.i(89148),r=e.i(919073),a=e.i(61732),o=e.i(465584);let l=(0,i.cvarsFrom)("KeyComboBlocks.module.css",["--font-size","--gap","--x-padding"]),u=e=>({Ctrl:e?"⌃":"Ctrl",Cmd:e?"⌘":"Cmd",Alt:e?"⌥":"Alt",Shift:e?"⇧":"Shift",Enter:"↵",ArrowLeft:"←",ArrowRight:"→",ArrowUp:"↑",ArrowDown:"↓"," ":"Space"});function d({text:e,small:s=!1,isActive:c=!1,hideBorder:p=!1,...m}){let h=(0,n.useIsMac)(),f=m.fontSize??(s?12:14),g=Math.round(1.4*f),y=Math.round(f*(p?.1:.2)),b=m.horizontalPadding??y,v={[l.fontSize]:f+"px",[l.xPadding]:b+"px",height:g,minWidth:p?void 0:g};c&&(v.fontWeight=i.tokens.fontWeightMedium);let S={borderColor:i.tokens.foregroundDefault,opacity:1};return(0,t.jsxs)(r.ShadesSurface,{elevate:!!c&&"1x",clsx:o.default.blockRoot,style:v,children:["string"==typeof e?(0,t.jsx)(a.View,{clsx:o.default.blockText,children:u(h)[e]??e}):e,p?null:(0,t.jsx)(a.View,{clsx:o.default.blockBorder,style:c?S:void 0,br:4})]})}e.s(["KeyComboBlocks",0,function e(n){if((0,s.isKeyChord)(n.keyCombo))return(0,t.jsx)(a.View,{"data-testid":`Keybinding:${n.keyCombo.join(" ")}`,clsx:o.default.chord,className:n.className,children:n.keyCombo.map((s,i)=>(0,t.jsx)(e,{...n,keyCombo:s},i))});let i=(0,s.keyCombinationOrPrefixToKeys)(n.keyCombo),r=n.fontSize??(n.small?12:14),u=n.hideBorder?0:Math.round(.2*r);return(0,t.jsx)(a.View,{"data-testid":`Keybinding:${n.keyCombo}`,clsx:o.default.shortcut,className:n.className,style:{[l.gap]:u+"px"},title:n.keyCombo,children:i.map(e=>(0,t.jsx)(d,{text:e,hideBorder:n.hideBorder,fontSize:r,horizontalPadding:n.horizontalPadding,isActive:n?.match?.includes(e.toLowerCase())},e))})},"keyComboText",0,function(e,t){return((0,s.isKeyChord)(e)?e:[e]).map(e=>(0,s.keyCombinationOrPrefixToKeys)(e).map(e=>u(t)[e]??e.toUpperCase()).join("")).join(" ")}])},119474,e=>{"use strict";var t=e.i(276385),n=e.i(389959),s=e.i(266556),i=e.i(295621),r=e.i(23818);function a(e){return(0,t.jsxs)(r.KeybindingsContext.Provider,{value:e.store,children:[e.children,(0,t.jsx)(d,{enabled:e.runnerEnabled??!0})]})}e.i(559357);let o=(0,n.createContext)(!1),l=[];function u(e){return(0,r.useRegisterCommands)(e.commands),(0,t.jsx)(t.Fragment,{children:e.children})}function d({enabled:e}){let t=(0,r.useResolveKeyCombo)(),a=(0,n.useRef)(null);return(0,n.useEffect)(function(){if(!e)return;let n=e=>{if(e.defaultPrevented){a.current=null;return}let n=(0,i.getKeyCombination)(e);if(!n)return;let o=n;a.current&&(o=(0,i.keyChord)(a.current,n));let l=t(o);switch(l.kind){case r.ResolutionResultKindEnum.NoMatch:a.current=null;break;case r.ResolutionResultKindEnum.WaitingForChord:if((0,s.isInputOrEditorFocused)()&&l.matches.every(({ignoreWhenInputFocused:e})=>e)){a.current=null;break}a.current=l.key,e.preventDefault();break;case r.ResolutionResultKindEnum.MatchFound:for(let{run:t,bubble:n=!0,ignoreWhenInputFocused:i}of(a.current=null,l.matches))if(!(i&&(0,s.isInputOrEditorFocused)())&&!1!==t()&&(e.preventDefault(),!n))break}};return document.addEventListener("keydown",n),()=>{document.removeEventListener("keydown",n)}},[e,t]),null}e.s(["AppKeybindingsProvider",0,function(e){let n=(0,r.useInitStore)(e);return(0,r.useFollowCurrentKeybindingsRow)(n),(0,r.useWaitForSavedKeybindings)(n,e.waitForSavedKeybindings??!1),(0,t.jsx)(a,{store:n,...e})},"AppLevelKeybindingsContext",0,o,"KeybindingsSurface",0,function(e){let s=(0,n.useContext)(o),i=(0,n.useContext)(r.KeybindingsContext),d=(0,r.useInitStore)(e);return(0,t.jsx)(a,{store:s&&i?i:d,runnerEnabled:!s,...e,children:(0,t.jsx)(u,{commands:s?e.commands:l,children:e.children})})}])},452317,e=>{"use strict";var t=e.i(389959),n=e.i(19777),s=e.i(330294),i=e.i(725169);let r=(0,t.createContext)(null);function a(){let e=(0,t.useContext)(r);if(!e)throw Error("Expected store in context");return e}e.s(["SessionContext",0,r,"useGet",0,function(e){return(0,s.useAtomCallback)((0,t.useCallback)(t=>t(e),[e]),{store:a()})},"useObservableValue",0,function(e){return(0,i.useObservableAtom)(e,a())},"useSet",0,e=>(0,n.useSetAtom)(e,{store:a()}),"useValue",0,function(e){return(0,n.useAtomValue)(e,{store:a()})}])},729245,e=>{"use strict";e.i(242933);let t=new(e.i(790164)).ObservableState(null),n=null;e.s(["activeTaskSource",0,t,"clearActiveTaskSource",0,function(e){n===e&&(n=null,t.set(null))},"markActiveTaskSourceReady",0,function(e){let s=t.current;n!==e||null===s||s.isReady||t.set({...s,isReady:!0})},"publishActiveTaskSource",0,function(e,s){n=e;let i=t.current;(null===i||i.isReady||i.replId!==s.replId||i.client!==s.client||i.sessionId!==s.sessionId)&&t.set({...s,isReady:!1})}])},267767,e=>{"use strict";e.s(["TEAMWORK_SYNC_COLLECTION_KEYS",0,["tasks","threads"]])},723349,e=>{"use strict";var t=e.i(267767);class n{syncedState=new Map;track(e,n){this.syncedState.has(e)||this.syncedState.set(e,function(){let e={};for(let n of t.TEAMWORK_SYNC_COLLECTION_KEYS)e[n]=new Map;return e}());let i=this.syncedState.get(e);return void 0===i?s():function(e,n){let i=s();for(let s of t.TEAMWORK_SYNC_COLLECTION_KEYS){var r,a,o;let t=function(e,t){let n=[];for(let s of t){let t=e.get(s.id);void 0!==t&&s.version<=t||(e.set(s.id,s.version),n.push(s))}return n}(e[s],n[s]??[]);r=i,a=s,o=t,r[a]=o}return i}(i,n)}clearShard(e){this.syncedState.delete(e)}reset(){this.syncedState.clear()}}function s(){let e={};for(let n of t.TEAMWORK_SYNC_COLLECTION_KEYS)e[n]=[];return e}e.s(["SyncStateTracker",0,n])},48208,e=>{"use strict";var t=e.i(723349);e.s(["CollectionSyncController",0,class{dataKey;params;callbacks;snapshotReplIds;_hasMarkedReady;pendingQueues;syncState;constructor(e,n,s){this.dataKey=e,this.params=n,this.callbacks=s,this.snapshotReplIds=new Set,this._hasMarkedReady=!1,this.pendingQueues=new Map,this.syncState=new t.SyncStateTracker}get hasSnapshot(){return this.snapshotReplIds.size>0}get hasMarkedReady(){return this._hasMarkedReady}beginTransaction(){this.params.begin()}commitTransaction(){this.params.commit()}applyMessage(e){if("snapshot"===e.type){for(let t of(this.applySnapshot(e.replId,e.snapshot),this.pendingQueues.get(e.replId)??[]))"documents"===t.type&&this.applyDocuments(t.replId,t.updates);this.pendingQueues.delete(e.replId)}else if("documents"===e.type)if(this.snapshotReplIds.has(e.replId))this.applyDocuments(e.replId,e.updates);else{let t=this.pendingQueues.get(e.replId)??[];t.push(e),this.pendingQueues.set(e.replId,t)}}markSyncReady(){this._hasMarkedReady=!0,this.params.markReady()}resetShard(e){this.snapshotReplIds.delete(e),this.pendingQueues.delete(e),this.syncState.clearShard(e)}applySnapshot(e,t){this.syncState.clearShard(e);let n=this.syncState.track(e,t);this.callbacks.processSnapshot(e,n[this.dataKey],{collection:this.params.collection,write:this.params.write}),this.snapshotReplIds.add(e)}applyDocuments(e,t){let n=this.syncState.track(e,t);this.callbacks.processDocuments(e,n[this.dataKey],{collection:this.params.collection,write:this.params.write})}}])},854246,e=>{"use strict";var t=e.i(830675);e.i(257697);var n=e.i(312806);e.i(242933);var s=e.i(424360),i=e.i(493830);e.i(292834);var r=e.i(16850),a=e.i(48208);class o{sessions=new Map;nextGeneration=0;pendingShardResets=new Set;$syncMessages=new i.Subject;syncMessages=s.StatelessMulticast.from(this.$syncMessages);collections=new Set;constructor(){this.syncMessages.subscribe(e=>{try{this.handleMessage(e)}catch(e){t.captureException(e)}})}registerCollectionForSync(e,t,n){let s=new a.CollectionSyncController(e,t,n);return this.collections.add(s),()=>{this.collections.delete(s)}}startSession(e){let{replId:s}=e;this.stopSession(s),this.pendingShardResets.add(s);let i=this.nextGeneration++,a=new AbortController,o={...e,generation:i,internalAbortController:a};this.sessions.set(s,o);let{externalAbortSignal:l,onSchemaValidationError:u}=o,d=new AbortController,c=()=>d.abort();l.addEventListener("abort",c),a.signal.addEventListener("abort",c);let p=d.signal;return(async()=>{try{let{resReadable:e}=o.client.realtime.workspaceData.subscribe({excludePlans:o.excludePlans??!1},{signal:p});for await(let t of e)if(t.ok&&this.isSessionActive(s,i)){if(!n.Value.Check(r.ReplFeedMessageSchema,t.payload)){u?.(),this.teardownSession(s,i);return}this.$syncMessages.next(t.payload),"snapshot"===t.payload.type&&t.payload.replId===s&&this.isSessionActive(s,i)&&o.onSnapshot?.()}}catch(e){p.aborted||t.captureException(e)}finally{l.removeEventListener("abort",c),a.signal.removeEventListener("abort",c),this.teardownSession(s,i)}})(),()=>this.teardownSession(s,i)}excludesPlansFor(e){return this.sessions.get(e)?.excludePlans??!1}stopSession(e){let t=this.sessions.get(e);t&&this.teardownSession(e,t.generation)}async transact(e){if(0===this.sessions.size)throw Error("Not connected to a Taskman session");return this.resolveOptimisticUpdate(await e())}async resolveOptimisticUpdate(e){let t=new Map,n=e=>{let n=t.get(e);if(n)return n;let s={tasks:[],planningSessions:[]};return t.set(e,s),s};for(let t of e.tasks??[])n(t.replId).tasks.push(t);for(let t of e.planningSessions??e.threads??[])n(t.replId).planningSessions.push(t);for(let[e,n]of t)this.sessions.has(e)&&this.$syncMessages.next({type:"documents",replId:e,updates:n});await Promise.resolve()}teardownSession(e,t){let n=this.sessions.get(e);n?.generation===t&&(n.internalAbortController.abort(),this.sessions.delete(e))}handleMessage(e){var t;let n="snapshot"===(t=e).type?{...t,snapshot:{...t.snapshot,threads:t.snapshot.planningSessions}}:{...t,updates:{...t.updates,threads:t.updates.planningSessions}},s=Array.from(this.collections.values());if(this.pendingShardResets.delete(e.replId))for(let t of s)t.resetShard(e.replId);for(let e of s)e.beginTransaction();for(let e of s)e.applyMessage(n);for(let e of s)e.commitTransaction();for(let e of s)e.hasSnapshot&&!e.hasMarkedReady&&e.markSyncReady()}isSessionActive(e,t){return this.sessions.get(e)?.generation===t}}let l=new o;e.s(["TeamworkSyncController",0,o,"teamworkSyncController",0,l])},927600,e=>{"use strict";var t=e.i(276385),n=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(n.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M15.53 11.47a.75.75 0 0 1 0 1.06l-6 6a.75.75 0 0 1-1.06-1.06L13.94 12 8.47 6.53a.75.75 0 0 1 1.06-1.06z",clipRule:"evenodd"})})}])},109591,e=>{"use strict";var t=e.i(276385),n=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(n.default,{...e,children:[(0,t.jsx)("path",{fillRule:"evenodd",d:"M8.927 8.054a2.334 2.334 0 0 1 2.858-1.65l9.416 2.523a2.334 2.334 0 0 1 1.65 2.858l-2.523 9.416a2.334 2.334 0 0 1-2.858 1.65l-9.416-2.523a2.334 2.334 0 0 1-1.65-2.858zm2.491-.28a.915.915 0 0 0-1.121.647l-2.524 9.416c-.13.488.16.99.648 1.121l9.416 2.523c.488.131.99-.159 1.121-.647l2.523-9.416a.916.916 0 0 0-.647-1.121z",clipRule:"evenodd"}),(0,t.jsx)("path",{d:"M13.232 1.15a2.334 2.334 0 0 1 2.334 2.334V4.89a.71.71 0 0 1-1.418 0V3.484a.916.916 0 0 0-.916-.916H3.484a.916.916 0 0 0-.916.916v9.748c0 .506.41.916.916.916H4.89a.71.71 0 1 1 0 1.418H3.484a2.334 2.334 0 0 1-2.334-2.334V3.484A2.334 2.334 0 0 1 3.484 1.15z"})]})}])},491194,e=>{"use strict";var t=e.i(276385),n=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(n.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M10 2.75A1.25 1.25 0 0 0 8.75 4v1.25h6.5V4A1.25 1.25 0 0 0 14 2.75zm6.75 2.5V4A2.75 2.75 0 0 0 14 1.25h-4A2.75 2.75 0 0 0 7.25 4v1.25H3a.75.75 0 0 0 0 1.5h1.25V20A2.75 2.75 0 0 0 7 22.75h10A2.75 2.75 0 0 0 19.75 20V6.75H21a.75.75 0 0 0 0-1.5zm-11 1.5V20A1.25 1.25 0 0 0 7 21.25h10A1.25 1.25 0 0 0 18.25 20V6.75zm4.25 3.5a.75.75 0 0 1 .75.75v6a.75.75 0 0 1-1.5 0v-6a.75.75 0 0 1 .75-.75m4 0a.75.75 0 0 1 .75.75v6a.75.75 0 0 1-1.5 0v-6a.75.75 0 0 1 .75-.75",clipRule:"evenodd"})})}])},521299,e=>{"use strict";var t=e.i(389959);e.s(["useIdSeed",0,()=>{let e=(0,t.useId)();return t=>`${e}-${t}`}])},601185,e=>{"use strict";let t="0123456789abcdefghjkmnpqrstvwxyz",n="0123456789abcdef",s=/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i,i=/^([a-z]([a-z_]{0,61}[a-z])?)?$/,r=/^[0-7][0-9abcdefghjkmnpqrstvwxyz]{25}$/;function a(e){if(!i.test(e))throw TypeError("TypeID prefix must match [a-z_]{0,63}")}function o(e){return e<=57?e-48:(32|e)-87}function l(e){return`${n[e>>>4]}${n[15&e]}`}e.s(["randomTypeIdToUuid",0,function(e,n){var s;let i=function(e,n){a(n);let s=""===n?"":"_",i=n.length+s.length;if(e.length!==i+26||!e.startsWith(`${n}${s}`))throw TypeError(`TypeID must have prefix "${n}"`);let o=e.slice(i);if(!r.test(o))throw TypeError("TypeID suffix is invalid");return function(e){let n=new Uint8Array(16),s=t.indexOf(e[0]),i=3,r=0;for(let a=1;a<e.length;a++)s=s<<5|t.indexOf(e[a]),(i+=5)>=8&&(i-=8,n[r]=s>>>i&255,r++,s&=(1<<i)-1);return n}(o)}(e,n);return function(e){if(e[6]>>>4!=4||(192&e[8])!=128)throw TypeError(`UUID must be a valid version ${4} UUID`)}(i),s=i,`${l(s[0])}${l(s[1])}${l(s[2])}${l(s[3])}-${l(s[4])}${l(s[5])}-${l(s[6])}${l(s[7])}-${l(s[8])}${l(s[9])}-${l(s[10])}${l(s[11])}${l(s[12])}${l(s[13])}${l(s[14])}${l(s[15])}`},"typeIdFromUuid",0,function(e,n){var i,r;let l;return i=e,r=function(e){if(!s.test(e))throw TypeError("UUID must use the canonical 8-4-4-4-12 format");let t=new Uint8Array(16),n=0;for(let s=0;s<t.length;s++)"-"===e[n]&&n++,t[s]=o(e.charCodeAt(n))<<4|o(e.charCodeAt(n+1)),n+=2;return t}(n),a(i),l=function(e){let n="",s=0,i=2;for(let r of e){for(s=s<<8|r,i+=8;i>=5;)i-=5,n+=t[s>>>i&31];s&=(1<<i)-1}return n}(r),""===i?l:`${i}_${l}`}])},629107,e=>{"use strict";var t=e.i(601185);let n=/(cnv_[0-7][0-9abcdefghjkmnpqrstvwxyz]{25})$/;e.s(["conversationIdFromUrlKey",0,function(e){let s=n.exec(e)?.[1];if(void 0===s)return e;try{return(0,t.randomTypeIdToUuid)(s,"cnv")}catch{return e}},"conversationUrlKey",0,function(e,n){let s=n?.split("-").slice(0,5).join("-")||"chat";return`${s}-${(0,t.typeIdFromUuid)("cnv",e)}`},"parseConversationReference",0,function(e){let n=/^([a-z0-9][a-z0-9-]*)-(cnv_[0-7][0-9abcdefghjkmnpqrstvwxyz]{25})$/.exec(e);if(!n)return null;try{return{conversationId:(0,t.randomTypeIdToUuid)(n[2],"cnv"),slug:n[1]}}catch{return null}}])}]);

//# debugId=a238420b-8c46-4257-d02d-1fe5ed3cd94e
//# sourceMappingURL=1o-x8zk2ywx_w.js.map