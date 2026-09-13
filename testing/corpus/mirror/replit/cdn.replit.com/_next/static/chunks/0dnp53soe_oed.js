;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="a59150ac-8dbd-2707-348c-ae614d726305")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,516458,e=>{"use strict";var t=e.i(724900);e.s(["RIVER_VERSION",()=>t.version])},992785,(e,t,n)=>{"use strict";t.exports=function e(t,n){if(t===n)return!0;if(t&&n&&"object"==typeof t&&"object"==typeof n){if(t.constructor!==n.constructor)return!1;if(Array.isArray(t)){if((r=t.length)!=n.length)return!1;for(i=r;0!=i--;)if(!e(t[i],n[i]))return!1;return!0}if(t instanceof Map&&n instanceof Map){if(t.size!==n.size)return!1;for(i of t.entries())if(!n.has(i[0]))return!1;for(i of t.entries())if(!e(i[1],n.get(i[0])))return!1;return!0}if(t instanceof Set&&n instanceof Set){if(t.size!==n.size)return!1;for(i of t.entries())if(!n.has(i[0]))return!1;return!0}if(ArrayBuffer.isView(t)&&ArrayBuffer.isView(n)){if((r=t.length)!=n.length)return!1;for(i=r;0!=i--;)if(t[i]!==n[i])return!1;return!0}if(t.constructor===RegExp)return t.source===n.source&&t.flags===n.flags;if(t.valueOf!==Object.prototype.valueOf)return t.valueOf()===n.valueOf();if(t.toString!==Object.prototype.toString)return t.toString()===n.toString();if((r=(s=Object.keys(t)).length)!==Object.keys(n).length)return!1;for(i=r;0!=i--;)if(!Object.prototype.hasOwnProperty.call(n,s[i]))return!1;for(i=r;0!=i--;){var r,i,s,o=s[i];if(!e(t[o],n[o]))return!1}return!0}return t!=t&&n!=n}},330294,e=>{"use strict";var t=e.i(389959),n=e.i(19777);e.i(485792);var r=e.i(602351);e.s(["useAtomCallback",0,function(e,i){let s=(0,t.useMemo)(()=>(0,r.atom)(null,(t,n,...r)=>e(t,n,...r)),[e]);return(0,n.useSetAtom)(s,i)}])},485792,e=>{"use strict";var t=e.i(602351);let n=Symbol(""),r=(e,t,n)=>(t.has(n)?t:t.set(n,e())).get(n),i=new WeakMap,s=e=>"function"==typeof(null==e?void 0:e.then);function o(e=()=>{try{return window.localStorage}catch(e){return}},t){var n;let r,i,a,l,u={getItem:(n,o)=>{var a,l;let u=e=>{if(r!==(e=e||"")){try{i=JSON.parse(e,null==t?void 0:t.reviver)}catch(e){return o}r=e}return i},d=null!=(l=null==(a=e())?void 0:a.getItem(n))?l:null;return s(d)?d.then(u):u(d)},setItem:(n,r)=>{var i;return null==(i=e())?void 0:i.setItem(n,JSON.stringify(r,null==t?void 0:t.replacer))},removeItem:t=>{var n;return null==(n=e())?void 0:n.removeItem(t)}};try{a=null==(n=e())?void 0:n.subscribe}catch(e){}return!a&&"u">typeof window&&"function"==typeof window.addEventListener&&window.Storage&&(a=(t,n)=>{if(!(e()instanceof window.Storage))return()=>{};let r=r=>{r.storageArea===e()&&r.key===t&&n(r.newValue)};return window.addEventListener("storage",r),()=>{window.removeEventListener("storage",r)}}),a&&(l=a,u.subscribe=(e,n,r)=>l(e,e=>{let i;try{i=JSON.parse(e||"",null==t?void 0:t.reviver)}catch(e){i=r}n(i)})),u}let a=o();e.s(["RESET",0,n,"atomFamily",0,function(e,t){let n=null,r=new Map,i=new Set,s=i=>{let a;if(void 0===t)a=r.get(i);else for(let[e,n]of r)if(t(e,i)){a=n;break}if(void 0!==a)if(null==n||!n(a[1],i))return a[0];else s.remove(i);let l=e(i);return r.set(i,[l,Date.now()]),o("CREATE",i,l),l},o=(e,t,n)=>{for(let r of i)r({type:e,param:t,atom:n})};return s.unstable_listen=e=>(i.add(e),()=>{i.delete(e)}),s.getParams=()=>r.keys(),s.remove=e=>{if(void 0===t){if(!r.has(e))return;let[t]=r.get(e);r.delete(e),o("REMOVE",e,t)}else for(let[n,[i]]of r)if(t(n,e)){r.delete(n),o("REMOVE",n,i);break}},s.setShouldRemove=e=>{if(n=e)for(let[e,[t,i]]of r)n(i,e)&&(r.delete(e),o("REMOVE",e,t))},s},"atomWithReset",0,function(e){let r=(0,t.atom)(e,(t,i,s)=>{let o="function"==typeof s?s(t(r)):s;i(r,o===n?e:o)});return r},"atomWithStorage",0,function(e,r,i=a,o){let l=null==o?void 0:o.getOnInit,u=(0,t.atom)(l?i.getItem(e,r):r);return u.onMount=t=>{var n;return t(i.getItem(e,r)),null==(n=i.subscribe)?void 0:n.call(i,e,t,r)},(0,t.atom)(e=>e(u),(t,o,a)=>{let l="function"==typeof a?a(t(u)):a;return l===n?(o(u,r),i.removeItem(e)):s(l)?l.then(t=>(o(u,t),i.setItem(e,t))):(o(u,l),i.setItem(e,l))})},"createJSONStorage",0,o,"selectAtom",0,function(e,n,s=Object.is){var o;let a,l;return o=()=>{let r=Symbol(),i=(0,t.atom)(t=>{let o=t(i);return(([e,t])=>{if(t===r)return n(e);let i=n(e,t);return s(t,i)?t:i})([t(e),o])});return i.init=r,i},a=r(()=>new WeakMap,i,e),l=r(()=>new WeakMap,a,n),r(o,l,s)}])},522624,e=>{"use strict";e.s(["urlAlphabet",0,"useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict"])},179104,e=>{"use strict";e.i(522624),e.s(["nanoid",0,(e=21)=>crypto.getRandomValues(new Uint8Array(e)).reduce((e,t)=>((t&=63)<36?e+=t.toString(36):t<62?e+=(t-26).toString(36).toUpperCase():t>62?e+="-":e+="_",e),"")])},954856,e=>{"use strict";for(var t=[],n=0;n<64;)t[n]=0|0x100000000*Math.sin(++n%Math.PI);for(var r,i=18,s=[],o=[];i>1;i--)for(r=i;r<320;)s[r+=i]=1;function a(e,t){return 0x100000000*Math.pow(e,1/t)|0}for(r=0;r<64;)s[++i]||(o[r]=a(i,2),s[r++]=a(i,3));function l(e,t){return e>>>t|e<<-t}e.s(["sha256",0,function(e){var t=o.slice(i=r=0,8),n=[],u=unescape(encodeURI(e))+"",d=u.length;for(n[e=--d/4+2|15]=8*d;~d;)n[d>>2]|=u.charCodeAt(d)<<8*~d--;for(d=[];i<e;i+=16){for(a=t.slice();r<64;a.unshift(u+(l(u=a[0],2)^l(u,13)^l(u,22))+(u&a[1]^a[1]&a[2]^a[2]&u)))a[3]+=u=0|(d[r]=r<16?~~n[r+i]:(l(u=d[r-2],17)^l(u,19)^u>>>10)+d[r-7]+(l(u=d[r-15],7)^l(u,18)^u>>>3)+d[r-16])+a.pop()+(l(u=a[4],6)^l(u,11)^l(u,25))+(u&a[5]^~u&a[6])+s[r++];for(r=8;r;)t[--r]+=a[r]}for(u="";r<64;)u+=(t[r>>3]>>4*(7-r++)&15).toString(16);return u}])},341732,e=>{"use strict";var t=e.i(389959),n=e.i(62624),r=e.i(796424),i=e.i(436298);e.s(["useSelectableArtifactKinds",0,function(e=!0){let s=null!==(0,t.useContext)(r.default),o=(0,t.useMemo)(()=>(0,i.buildSelectableArtifactKinds)({isZealot:s}),[s]),a=(0,n.useMobileOutputOptions)(o);return e?a:o}])},668721,e=>{"use strict";var t=e.i(351623);let n=t.gql`
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
    `;e.s(["DeploymentLinkFragmentDoc",0,n])},676107,e=>{"use strict";var t=e.i(351623),n=e.i(668721),r=e.i(323604),i=e.i(319801);let s=t.gql`
    fragment ReplDomain2 on Domain {
  id
  hosting_deployment_id
  domain
  state
}
    `,o=t.gql`
    fragment CustomDomain on Domain {
  ...ReplDomain2
}
    ${s}`,a=t.gql`
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
${o}
${a}
${r.DeploymentLinkFragmentDoc}
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
${r.DeploymentLinkFragmentDoc}
${c}`;e.s(["CurrentBuild2FragmentDoc",0,d,"DeploymentItemFragmentDoc",0,p,"HostingBuildArtifactFieldsFragmentDoc",0,u,"MachineConfigurationFragmentDoc",0,l,"TargetHostingDeploymentFragmentDoc",0,a])},631567,e=>{"use strict";var t=e.i(351623),n=e.i(344480);e.i(975473);let r={},i=t.gql`
    query CurrentUserId {
  currentUser {
    id
  }
}
    `;e.s(["useCurrentUserIdQuery",0,function(e){let t={...r,...e};return n.useQuery(i,t)}])},570438,e=>{"use strict";var t=e.i(631567);e.s(["useCurrentUserId",0,function({skip:e}={}){let{data:n}=(0,t.useCurrentUserIdQuery)({skip:e});return n?.currentUser?.id??null},"useCurrentUserIdState",0,function({skip:e}={}){let{data:n,loading:r,error:i}=(0,t.useCurrentUserIdQuery)({skip:e});return{currentUserId:n?.currentUser?.id??null,isCurrentUserIdLoading:r,isCurrentUserResolved:!r&&!i&&void 0!==n}}])},62624,e=>{"use strict";var t=e.i(389959),n=e.i(436298),r=e.i(753451);e.s(["useMobileOutputOptions",0,function(e){let i=(0,r.useDoesBonsaiSupportFeature)("disableMobileAppCreationOniOS"),s="ios"===(0,r.useMobileAppPlatform)(),o=i&&s;return(0,t.useMemo)(()=>(0,n.getMobileOutputOptions)({excludeMobileArtifact:o,kinds:e}),[o,e])}])},473072,e=>{"use strict";var t=e.i(389959),n=e.i(796424);e.s(["default",0,function(){let e=(0,t.useContext)(n.default);if(!e)throw Error("Expected Repl ID to be in context");return e}])},892158,e=>{"use strict";var t=e.i(276385),n=e.i(36454),r=e.i(389959),i=e.i(405411),s=e.i(983420),o=e.i(152651),a=e.i(210853),l=e.i(406664),u=e.i(158627),d=e.i(27923),c=e.i(488299);let p=e.i(61732).SpecializedView.a,[f,m]=(0,d.twVar)({variable:"--icon-button-size"}),h=(0,r.forwardRef)(function(e,r){let{props:i,className:h,styles:g,attributes:y}=(0,u.useRuiComponentProps)("IconButtonLink",e),{alt:v,children:b,colorway:S,disabled:C,size:k=c.defaultIconSize,as:w,href:R,prefetch:x,replace:I,scroll:M,shallow:A,dataCy:D,variant:E,filled:j,onClick:O,onAuxClick:T,..._}=i,K=(0,a.useClickIntentHandlers)({onClick:O,onAuxClick:T}),B=(0,l.useCreateInteractive)({variant:E??(j?"filled":"nofill"),colorway:S,disabled:C}),L={...y,disabled:C,style:{...m(`${k}px`),...B.style,...g},className:d.tw.merge("flex justify-center items-center",d.tw.var(f("w-(--icon-button-size) h-(--icon-button-size)")),d.tw.external(B.clsx),d.tw.external(h)),ref:r,..._},P=(0,t.jsx)(s.IconProvider,{alt:v,size:c.IconSizeMap[k],children:b});return C?(0,t.jsx)(p,{"data-cy":D,"aria-disabled":C,role:"link",...L,children:P}):(0,t.jsx)(n.default,{"data-cy":D,role:"link",...o.INSTRUMENTED_MARKER_PROP,as:w,href:R,prefetch:x,replace:I,scroll:M,shallow:A,...L,...K,children:P})}),g=(0,r.forwardRef)(function(e,n){let{alt:r,tooltipBehavior:s,tooltipPlacement:o,tooltipContents:a,...l}=e;return(0,t.jsxs)(i.TooltipTrigger,{isDisabled:"hidden"===s,children:[(0,t.jsx)(h,{alt:r,ref:n,...l}),(0,t.jsx)(c.IconButtonTooltip,{placement:o,children:a??r})]})});e.s(["IconButtonLink",0,g])},66742,e=>{"use strict";var t=e.i(276385),n=e.i(152651),r=e.i(210853),i=e.i(532225),s=e.i(158627),o=e.i(744006),a=e.i(223620);e.s(["PillButton",0,function(e){let{props:l,className:u,styles:d,attributes:c}=(0,s.useRuiComponentProps)("PillButton",e),{colorway:p,onClick:f,onAuxClick:m,...h}=l,g=(0,r.useClickIntentHandlers)({onClick:f,onAuxClick:m}),y=(0,i.useCreateInteractiveRuiClasses)({variant:"filled",colorway:p??"default",borderRadius:h.compact?"md":"full",focusRingBehavior:"onCustomFocus",cursorWhenEnabled:"pointer"});return(0,t.jsx)(o.Pill,{...c,...h,colorway:p,tag:"button",cursorWhenEnabled:"pointer",...n.INSTRUMENTED_MARKER_PROP,...g,clsx:(0,a.twMerge)(y,u),style:d})}])},114953,e=>{"use strict";e.i(242933);let t=new(e.i(790164)).ObservableState(null),n=0;e.s(["clearReplMainAgentTarget",0,function(e){t.current?.kind==="target"&&t.current.target.replId===e&&t.set(null)},"invalidateReplMainAgentTarget",0,function(e){n+=1,t.set({kind:"evicted",replId:e,version:n})},"replMainAgentTargetState",0,t,"resolveReplMainAgentInvalidation",0,function(e,n){t.current?.kind==="evicted"&&t.current.replId===e&&t.current.version===n&&t.set(null)},"setReplMainAgentTarget",0,function(e){let n=t.current;(n?.kind!=="target"||n.target.replId!==e.replId||n.target.agentId!==e.agentId||n.target.endpoint!==e.endpoint)&&t.set({kind:"target",target:e})}])},124575,e=>{e.v({submenuContainer:"NewArtifactSubmenu-module__2jvkpW__submenuContainer"})},280810,e=>{"use strict";var t=e.i(276385),n=e.i(625251),r=e.i(927600),i=e.i(109591),s=e.i(436298),o=e.i(341732);e.i(214847);var a=e.i(864300),l=e.i(919073),u=e.i(295231),d=e.i(773222),c=e.i(124575);function p({kind:e,onSelect:n}){let r=(0,a.useIntl)(),i=(0,s.getOutputKindConfig)(e),o=(0,s.getOutputKindLabel)(r,e);return(0,t.jsx)(u.MenuItem,{label:o,icon:(0,t.jsx)(i.Icon,{}),onAction:()=>n(e),dataCy:`artifact-picker-pill-${i.label.toLowerCase().replace(/\s+/g,"-")}`})}function f({onSelect:e,kinds:n,footer:r}){let i=(0,o.useSelectableArtifactKinds)();return(0,t.jsxs)(t.Fragment,{children:[(n??i).map(n=>(0,t.jsx)(p,{kind:n,onSelect:e},(0,s.outputKindKey)(n))),r]})}e.s(["NewArtifactPickerMenuItems",0,f,"NewArtifactSubmenu",0,function({onSelect:e,kinds:s,footer:o,onOpen:p,placement:m}){let h=(0,a.useIntl)();return(0,t.jsxs)(n.SubmenuTrigger,{delay:0,children:[(0,t.jsx)(u.MenuItem,{label:h.formatMessage({id:"workspace.newArtifactSubmenuTriggerLabel",defaultMessage:"Create something new"}),icon:(0,t.jsx)(i.default,{}),iconRight:(0,t.jsx)(r.default,{}),onPressStart:()=>p?.()}),(0,t.jsx)(d.RawPopover,{offset:4,placement:m,children:(0,t.jsx)(l.ShadesSurface,{colorShade:"themePopup",p:4,br:"container",clsx:c.default.submenuContainer,children:(0,t.jsx)(u.Menu,{"aria-label":h.formatMessage({id:"workspace.newArtifactSubmenuAriaLabel",defaultMessage:"Create something new"}),children:(0,t.jsx)(f,{onSelect:e,kinds:s,footer:o})})})})]})}])},465584,e=>{e.v({blockBorder:"KeyComboBlocks-module__3yf3FW__blockBorder",blockRoot:"KeyComboBlocks-module__3yf3FW__blockRoot",blockText:"KeyComboBlocks-module__3yf3FW__blockText",chord:"KeyComboBlocks-module__3yf3FW__chord",shortcut:"KeyComboBlocks-module__3yf3FW__shortcut"})},559357,e=>{"use strict";var t=e.i(276385),n=e.i(68701),r=e.i(295621),i=e.i(89148),s=e.i(919073),o=e.i(61732),a=e.i(465584);let l=(0,i.cvarsFrom)("KeyComboBlocks.module.css",["--font-size","--gap","--x-padding"]),u=e=>({Ctrl:e?"⌃":"Ctrl",Cmd:e?"⌘":"Cmd",Alt:e?"⌥":"Alt",Shift:e?"⇧":"Shift",Enter:"↵",ArrowLeft:"←",ArrowRight:"→",ArrowUp:"↑",ArrowDown:"↓"," ":"Space"});function d({text:e,small:r=!1,isActive:c=!1,hideBorder:p=!1,...f}){let m=(0,n.useIsMac)(),h=f.fontSize??(r?12:14),g=Math.round(1.4*h),y=Math.round(h*(p?.1:.2)),v=f.horizontalPadding??y,b={[l.fontSize]:h+"px",[l.xPadding]:v+"px",height:g,minWidth:p?void 0:g};c&&(b.fontWeight=i.tokens.fontWeightMedium);let S={borderColor:i.tokens.foregroundDefault,opacity:1};return(0,t.jsxs)(s.ShadesSurface,{elevate:!!c&&"1x",clsx:a.default.blockRoot,style:b,children:["string"==typeof e?(0,t.jsx)(o.View,{clsx:a.default.blockText,children:u(m)[e]??e}):e,p?null:(0,t.jsx)(o.View,{clsx:a.default.blockBorder,style:c?S:void 0,br:4})]})}e.s(["KeyComboBlocks",0,function e(n){if((0,r.isKeyChord)(n.keyCombo))return(0,t.jsx)(o.View,{"data-testid":`Keybinding:${n.keyCombo.join(" ")}`,clsx:a.default.chord,className:n.className,children:n.keyCombo.map((r,i)=>(0,t.jsx)(e,{...n,keyCombo:r},i))});let i=(0,r.keyCombinationOrPrefixToKeys)(n.keyCombo),s=n.fontSize??(n.small?12:14),u=n.hideBorder?0:Math.round(.2*s);return(0,t.jsx)(o.View,{"data-testid":`Keybinding:${n.keyCombo}`,clsx:a.default.shortcut,className:n.className,style:{[l.gap]:u+"px"},title:n.keyCombo,children:i.map(e=>(0,t.jsx)(d,{text:e,hideBorder:n.hideBorder,fontSize:s,horizontalPadding:n.horizontalPadding,isActive:n?.match?.includes(e.toLowerCase())},e))})},"keyComboText",0,function(e,t){return((0,r.isKeyChord)(e)?e:[e]).map(e=>(0,r.keyCombinationOrPrefixToKeys)(e).map(e=>u(t)[e]??e.toUpperCase()).join("")).join(" ")}])},119474,e=>{"use strict";var t=e.i(276385),n=e.i(389959),r=e.i(266556),i=e.i(295621),s=e.i(23818);function o(e){return(0,t.jsxs)(s.KeybindingsContext.Provider,{value:e.store,children:[e.children,(0,t.jsx)(d,{enabled:e.runnerEnabled??!0})]})}e.i(559357);let a=(0,n.createContext)(!1),l=[];function u(e){return(0,s.useRegisterCommands)(e.commands),(0,t.jsx)(t.Fragment,{children:e.children})}function d({enabled:e}){let t=(0,s.useResolveKeyCombo)(),o=(0,n.useRef)(null);return(0,n.useEffect)(function(){if(!e)return;let n=e=>{if(e.defaultPrevented){o.current=null;return}let n=(0,i.getKeyCombination)(e);if(!n)return;let a=n;o.current&&(a=(0,i.keyChord)(o.current,n));let l=t(a);switch(l.kind){case s.ResolutionResultKindEnum.NoMatch:o.current=null;break;case s.ResolutionResultKindEnum.WaitingForChord:if((0,r.isInputOrEditorFocused)()&&l.matches.every(({ignoreWhenInputFocused:e})=>e)){o.current=null;break}o.current=l.key,e.preventDefault();break;case s.ResolutionResultKindEnum.MatchFound:for(let{run:t,bubble:n=!0,ignoreWhenInputFocused:i}of(o.current=null,l.matches))if(!(i&&(0,r.isInputOrEditorFocused)())&&!1!==t()&&(e.preventDefault(),!n))break}};return document.addEventListener("keydown",n),()=>{document.removeEventListener("keydown",n)}},[e,t]),null}e.s(["AppKeybindingsProvider",0,function(e){let n=(0,s.useInitStore)(e);return(0,s.useFollowCurrentKeybindingsRow)(n),(0,s.useWaitForSavedKeybindings)(n,e.waitForSavedKeybindings??!1),(0,t.jsx)(o,{store:n,...e})},"AppLevelKeybindingsContext",0,a,"KeybindingsSurface",0,function(e){let r=(0,n.useContext)(a),i=(0,n.useContext)(s.KeybindingsContext),d=(0,s.useInitStore)(e);return(0,t.jsx)(o,{store:r&&i?i:d,runnerEnabled:!r,...e,children:(0,t.jsx)(u,{commands:r?e.commands:l,children:e.children})})}])},729245,e=>{"use strict";e.i(242933);let t=new(e.i(790164)).ObservableState(null),n=null;e.s(["activeTaskSource",0,t,"clearActiveTaskSource",0,function(e){n===e&&(n=null,t.set(null))},"markActiveTaskSourceReady",0,function(e){let r=t.current;n!==e||null===r||r.isReady||t.set({...r,isReady:!0})},"publishActiveTaskSource",0,function(e,r){n=e;let i=t.current;(null===i||i.isReady||i.replId!==r.replId||i.client!==r.client||i.sessionId!==r.sessionId)&&t.set({...r,isReady:!1})}])},267767,e=>{"use strict";e.s(["TEAMWORK_SYNC_COLLECTION_KEYS",0,["tasks","threads"]])},723349,e=>{"use strict";var t=e.i(267767);class n{syncedState=new Map;track(e,n){this.syncedState.has(e)||this.syncedState.set(e,function(){let e={};for(let n of t.TEAMWORK_SYNC_COLLECTION_KEYS)e[n]=new Map;return e}());let i=this.syncedState.get(e);return void 0===i?r():function(e,n){let i=r();for(let r of t.TEAMWORK_SYNC_COLLECTION_KEYS){var s,o,a;let t=function(e,t){let n=[];for(let r of t){let t=e.get(r.id);void 0!==t&&r.version<=t||(e.set(r.id,r.version),n.push(r))}return n}(e[r],n[r]??[]);s=i,o=r,a=t,s[o]=a}return i}(i,n)}clearShard(e){this.syncedState.delete(e)}reset(){this.syncedState.clear()}}function r(){let e={};for(let n of t.TEAMWORK_SYNC_COLLECTION_KEYS)e[n]=[];return e}e.s(["SyncStateTracker",0,n])},48208,e=>{"use strict";var t=e.i(723349);e.s(["CollectionSyncController",0,class{dataKey;params;callbacks;snapshotReplIds;_hasMarkedReady;pendingQueues;syncState;constructor(e,n,r){this.dataKey=e,this.params=n,this.callbacks=r,this.snapshotReplIds=new Set,this._hasMarkedReady=!1,this.pendingQueues=new Map,this.syncState=new t.SyncStateTracker}get hasSnapshot(){return this.snapshotReplIds.size>0}get hasMarkedReady(){return this._hasMarkedReady}beginTransaction(){this.params.begin()}commitTransaction(){this.params.commit()}applyMessage(e){if("snapshot"===e.type){for(let t of(this.applySnapshot(e.replId,e.snapshot),this.pendingQueues.get(e.replId)??[]))"documents"===t.type&&this.applyDocuments(t.replId,t.updates);this.pendingQueues.delete(e.replId)}else if("documents"===e.type)if(this.snapshotReplIds.has(e.replId))this.applyDocuments(e.replId,e.updates);else{let t=this.pendingQueues.get(e.replId)??[];t.push(e),this.pendingQueues.set(e.replId,t)}}markSyncReady(){this._hasMarkedReady=!0,this.params.markReady()}resetShard(e){this.snapshotReplIds.delete(e),this.pendingQueues.delete(e),this.syncState.clearShard(e)}applySnapshot(e,t){this.syncState.clearShard(e);let n=this.syncState.track(e,t);this.callbacks.processSnapshot(e,n[this.dataKey],{collection:this.params.collection,write:this.params.write}),this.snapshotReplIds.add(e)}applyDocuments(e,t){let n=this.syncState.track(e,t);this.callbacks.processDocuments(e,n[this.dataKey],{collection:this.params.collection,write:this.params.write})}}])},854246,e=>{"use strict";var t=e.i(830675);e.i(257697);var n=e.i(312806);e.i(242933);var r=e.i(424360),i=e.i(493830);e.i(292834);var s=e.i(16850),o=e.i(48208);class a{sessions=new Map;nextGeneration=0;pendingShardResets=new Set;$syncMessages=new i.Subject;syncMessages=r.StatelessMulticast.from(this.$syncMessages);collections=new Set;constructor(){this.syncMessages.subscribe(e=>{try{this.handleMessage(e)}catch(e){t.captureException(e)}})}registerCollectionForSync(e,t,n){let r=new o.CollectionSyncController(e,t,n);return this.collections.add(r),()=>{this.collections.delete(r)}}startSession(e){let{replId:r}=e;this.stopSession(r),this.pendingShardResets.add(r);let i=this.nextGeneration++,o=new AbortController,a={...e,generation:i,internalAbortController:o};this.sessions.set(r,a);let{externalAbortSignal:l,onSchemaValidationError:u}=a,d=new AbortController,c=()=>d.abort();l.addEventListener("abort",c),o.signal.addEventListener("abort",c);let p=d.signal;return(async()=>{try{let{resReadable:e}=a.client.realtime.workspaceData.subscribe({excludePlans:a.excludePlans??!1},{signal:p});for await(let t of e)if(t.ok&&this.isSessionActive(r,i)){if(!n.Value.Check(s.ReplFeedMessageSchema,t.payload)){u?.(),this.teardownSession(r,i);return}this.$syncMessages.next(t.payload),"snapshot"===t.payload.type&&t.payload.replId===r&&this.isSessionActive(r,i)&&a.onSnapshot?.()}}catch(e){p.aborted||t.captureException(e)}finally{l.removeEventListener("abort",c),o.signal.removeEventListener("abort",c),this.teardownSession(r,i)}})(),()=>this.teardownSession(r,i)}excludesPlansFor(e){return this.sessions.get(e)?.excludePlans??!1}stopSession(e){let t=this.sessions.get(e);t&&this.teardownSession(e,t.generation)}async transact(e){if(0===this.sessions.size)throw Error("Not connected to a Taskman session");return this.resolveOptimisticUpdate(await e())}async resolveOptimisticUpdate(e){let t=new Map,n=e=>{let n=t.get(e);if(n)return n;let r={tasks:[],planningSessions:[]};return t.set(e,r),r};for(let t of e.tasks??[])n(t.replId).tasks.push(t);for(let t of e.planningSessions??e.threads??[])n(t.replId).planningSessions.push(t);for(let[e,n]of t)this.sessions.has(e)&&this.$syncMessages.next({type:"documents",replId:e,updates:n});await Promise.resolve()}teardownSession(e,t){let n=this.sessions.get(e);n?.generation===t&&(n.internalAbortController.abort(),this.sessions.delete(e))}handleMessage(e){var t;let n="snapshot"===(t=e).type?{...t,snapshot:{...t.snapshot,threads:t.snapshot.planningSessions}}:{...t,updates:{...t.updates,threads:t.updates.planningSessions}},r=Array.from(this.collections.values());if(this.pendingShardResets.delete(e.replId))for(let t of r)t.resetShard(e.replId);for(let e of r)e.beginTransaction();for(let e of r)e.applyMessage(n);for(let e of r)e.commitTransaction();for(let e of r)e.hasSnapshot&&!e.hasMarkedReady&&e.markSyncReady()}isSessionActive(e,t){return this.sessions.get(e)?.generation===t}}let l=new a;e.s(["TeamworkSyncController",0,a,"teamworkSyncController",0,l])},304151,e=>{"use strict";var t=e.i(276385),n=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(n.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 2.25a.75.75 0 0 1 .75.75v10.19l3.72-3.72a.75.75 0 1 1 1.06 1.06l-5 5a.75.75 0 0 1-1.06 0l-5-5a.75.75 0 1 1 1.06-1.06l3.72 3.72V3a.75.75 0 0 1 .75-.75m-9 12a.75.75 0 0 1 .75.75v4A1.25 1.25 0 0 0 5 20.25h14A1.25 1.25 0 0 0 20.25 19v-4a.75.75 0 0 1 1.5 0v4A2.75 2.75 0 0 1 19 21.75H5A2.75 2.75 0 0 1 2.25 19v-4a.75.75 0 0 1 .75-.75",clipRule:"evenodd"})})}])},927225,e=>{"use strict";var t=e.i(276385),n=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(n.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M3.25 12a1.75 1.75 0 1 1 3.5 0 1.75 1.75 0 0 1-3.5 0m7 0a1.75 1.75 0 1 1 3.5 0 1.75 1.75 0 0 1-3.5 0m7 0a1.75 1.75 0 1 1 3.5 0 1.75 1.75 0 0 1-3.5 0",clipRule:"evenodd"})})}])},109591,e=>{"use strict";var t=e.i(276385),n=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(n.default,{...e,children:[(0,t.jsx)("path",{fillRule:"evenodd",d:"M8.927 8.054a2.334 2.334 0 0 1 2.858-1.65l9.416 2.523a2.334 2.334 0 0 1 1.65 2.858l-2.523 9.416a2.334 2.334 0 0 1-2.858 1.65l-9.416-2.523a2.334 2.334 0 0 1-1.65-2.858zm2.491-.28a.915.915 0 0 0-1.121.647l-2.524 9.416c-.13.488.16.99.648 1.121l9.416 2.523c.488.131.99-.159 1.121-.647l2.523-9.416a.916.916 0 0 0-.647-1.121z",clipRule:"evenodd"}),(0,t.jsx)("path",{d:"M13.232 1.15a2.334 2.334 0 0 1 2.334 2.334V4.89a.71.71 0 0 1-1.418 0V3.484a.916.916 0 0 0-.916-.916H3.484a.916.916 0 0 0-.916.916v9.748c0 .506.41.916.916.916H4.89a.71.71 0 1 1 0 1.418H3.484a2.334 2.334 0 0 1-2.334-2.334V3.484A2.334 2.334 0 0 1 3.484 1.15z"})]})}])},491194,e=>{"use strict";var t=e.i(276385),n=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(n.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M10 2.75A1.25 1.25 0 0 0 8.75 4v1.25h6.5V4A1.25 1.25 0 0 0 14 2.75zm6.75 2.5V4A2.75 2.75 0 0 0 14 1.25h-4A2.75 2.75 0 0 0 7.25 4v1.25H3a.75.75 0 0 0 0 1.5h1.25V20A2.75 2.75 0 0 0 7 22.75h10A2.75 2.75 0 0 0 19.75 20V6.75H21a.75.75 0 0 0 0-1.5zm-11 1.5V20A1.25 1.25 0 0 0 7 21.25h10A1.25 1.25 0 0 0 18.25 20V6.75zm4.25 3.5a.75.75 0 0 1 .75.75v6a.75.75 0 0 1-1.5 0v-6a.75.75 0 0 1 .75-.75m4 0a.75.75 0 0 1 .75.75v6a.75.75 0 0 1-1.5 0v-6a.75.75 0 0 1 .75-.75",clipRule:"evenodd"})})}])},826771,e=>{"use strict";e.s(["observableValue",0,function(e){let t=e,n=new Map;function r(e){if(t!==e)for(let r of(t=e,n.values()))r(e)}return{get current(){return t},set current(newValue){r(newValue)},set:r,update(e){r(e(t))},subscribe(e){let t=()=>n.delete(t);return n.set(t,e),t}}}])}]);

//# debugId=a59150ac-8dbd-2707-348c-ae614d726305
//# sourceMappingURL=1r-jq-hdiidw_.js.map