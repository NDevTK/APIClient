;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="6d14cde8-1f7b-7008-2c81-27a233a18455")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,284733,(e,t,r)=>{"use strict";function n(e,t,r,n){return!1}Object.defineProperty(r,"__esModule",{value:!0}),Object.defineProperty(r,"getDomainLocale",{enumerable:!0,get:function(){return n}}),e.r(77034),("function"==typeof r.default||"object"==typeof r.default&&null!==r.default)&&void 0===r.default.__esModule&&(Object.defineProperty(r.default,"__esModule",{value:!0}),Object.assign(r.default,r),t.exports=r.default)},468292,(e,t,r)=>{"use strict";Object.defineProperty(r,"__esModule",{value:!0});var n={default:function(){return C},useLinkStatus:function(){return b}};for(var i in n)Object.defineProperty(r,i,{enumerable:!0,get:n[i]});let a=e.r(887602),o=e.r(478902),s=a._(e.r(389959)),l=e.r(57406),u=e.r(211493),c=e.r(329827),d=e.r(418720),p=e.r(543811),f=e.r(901518),m=e.r(271448),g=e.r(284733),h=e.r(346570),v=e.r(801220),y=new Set;function _(e,t,r,n){if(!("u"<typeof window)&&(0,u.isLocalURL)(t)){if(!n.bypassPrefetchedCheck){let i=t+"%"+r+"%"+(void 0!==n.locale?n.locale:"locale"in e?e.locale:void 0);if(y.has(i))return;y.add(i)}e.prefetch(t,r,n).catch(e=>{})}}function T(e){return"string"==typeof e?e:(0,c.formatUrl)(e)}let P=s.default.forwardRef(function(e,t){let r,n,{href:i,as:a,children:c,prefetch:y=null,passHref:P,replace:E,shallow:b,scroll:C,locale:w,onClick:R,onNavigate:x,onMouseEnter:I,onTouchStart:A,legacyBehavior:S=!1,transitionTypes:O,...U}=e;r=c,S&&("string"==typeof r||"number"==typeof r)&&(r=(0,o.jsx)("a",{children:r}));let k=s.default.useContext(f.RouterContext),M=!1!==y,{href:F,as:D}=s.default.useMemo(()=>{if(!k){let e=T(i);return{href:e,as:a?T(a):e}}let[e,t]=(0,l.resolveHref)(k,i,!0);return{href:e,as:a?(0,l.resolveHref)(k,a):t||e}},[k,i,a]),L=s.default.useRef(F),j=s.default.useRef(D);S&&(n=s.default.Children.only(r));let z=S?n&&"object"==typeof n&&n.ref:t,[N,H,V]=(0,m.useIntersection)({rootMargin:"200px"}),B=s.default.useCallback(e=>{(j.current!==D||L.current!==F)&&(V(),j.current=D,L.current=F),N(e)},[D,F,V,N]),W=(0,v.useMergedRef)(B,z);s.default.useEffect(()=>{!k||H&&M&&_(k,F,D,{bypassPrefetchedCheck:!1,locale:w})},[D,F,H,w,M,k?.locale,k]);let $={ref:W,onClick(e){S||"function"!=typeof R||R(e),S&&n.props&&"function"==typeof n.props.onClick&&n.props.onClick(e),!k||e.defaultPrevented||function(e,t,r,n,i,a,o,s,l){let c,{nodeName:d}=e.currentTarget;if(!("A"===d.toUpperCase()&&((c=e.currentTarget.getAttribute("target"))&&"_self"!==c||e.metaKey||e.ctrlKey||e.shiftKey||e.altKey||e.nativeEvent&&2===e.nativeEvent.which)||e.currentTarget.hasAttribute("download"))){if(!(0,u.isLocalURL)(r)){i&&(e.preventDefault(),location.replace(r));return}e.preventDefault(),(()=>{if(l){let e=!1;if(l({preventDefault:()=>{e=!0}}),e)return}let e=o??!0;"beforePopState"in t?t[i?"replace":"push"](r,n,{shallow:a,locale:s,scroll:e}):t[i?"replace":"push"](n||r,{scroll:e})})()}}(e,k,F,D,E,b,C,w,x)},onMouseEnter(e){S||"function"!=typeof I||I(e),S&&n.props&&"function"==typeof n.props.onMouseEnter&&n.props.onMouseEnter(e),k&&_(k,F,D,{locale:w,priority:!0,bypassPrefetchedCheck:!0})},onTouchStart:function(e){S||"function"!=typeof A||A(e),S&&n.props&&"function"==typeof n.props.onTouchStart&&n.props.onTouchStart(e),k&&_(k,F,D,{locale:w,priority:!0,bypassPrefetchedCheck:!0})}};if((0,d.isAbsoluteUrl)(D))$.href=D;else if(!S||P||"a"===n.type&&!("href"in n.props)){let e=void 0!==w?w:k?.locale;$.href=k?.isLocaleDomain&&(0,g.getDomainLocale)(D,e,k?.locales,k?.domainLocales)||(0,h.addBasePath)((0,p.addLocale)(D,e,k?.defaultLocale))}return S?s.default.cloneElement(n,$):(0,o.jsx)("a",{...U,...$,children:r})}),E=(0,s.createContext)({pending:!1}),b=()=>(0,s.useContext)(E),C=P;("function"==typeof r.default||"object"==typeof r.default&&null!==r.default)&&void 0===r.default.__esModule&&(Object.defineProperty(r.default,"__esModule",{value:!0}),Object.assign(r.default,r),t.exports=r.default)},36454,(e,t,r)=>{t.exports=e.r(468292)},271448,(e,t,r)=>{"use strict";Object.defineProperty(r,"__esModule",{value:!0}),Object.defineProperty(r,"useIntersection",{enumerable:!0,get:function(){return l}});let n=e.r(389959),i=e.r(778747),a="function"==typeof IntersectionObserver,o=new Map,s=[];function l({rootRef:e,rootMargin:t,disabled:r}){let u=r||!a,[c,d]=(0,n.useState)(!1),p=(0,n.useRef)(null),f=(0,n.useCallback)(e=>{p.current=e},[]);return(0,n.useEffect)(()=>{if(a){if(u||c)return;let r=p.current;if(r&&r.tagName)return function(e,t,r){let{id:n,observer:i,elements:a}=function(e){let t,r={root:e.root||null,margin:e.rootMargin||""},n=s.find(e=>e.root===r.root&&e.margin===r.margin);if(n&&(t=o.get(n)))return t;let i=new Map;return t={id:r,observer:new IntersectionObserver(e=>{e.forEach(e=>{let t=i.get(e.target),r=e.isIntersecting||e.intersectionRatio>0;t&&r&&t(r)})},e),elements:i},s.push(r),o.set(r,t),t}(r);return a.set(e,t),i.observe(e),function(){if(a.delete(e),i.unobserve(e),0===a.size){i.disconnect(),o.delete(n);let e=s.findIndex(e=>e.root===n.root&&e.margin===n.margin);e>-1&&s.splice(e,1)}}}(r,e=>e&&d(e),{root:e?.current,rootMargin:t})}else if(!c){let e=(0,i.requestIdleCallback)(()=>d(!0));return()=>(0,i.cancelIdleCallback)(e)}},[u,t,e,c,p.current]),[f,c,(0,n.useCallback)(()=>{d(!1)},[])]}("function"==typeof r.default||"object"==typeof r.default&&null!==r.default)&&void 0===r.default.__esModule&&(Object.defineProperty(r.default,"__esModule",{value:!0}),Object.assign(r.default,r),t.exports=r.default)},223481,e=>{"use strict";var t=e.i(105349);e.s(["validate",()=>t.default])},892663,e=>{"use strict";let t=new Map,r=new Set,n=null,i=!1;function a(e){for(let n of e){let e=n.target,a=r.has(e);n.isIntersecting!==a&&(n.isIntersecting?(r.add(e),i||t.get(e)?.onEnter()):(r.delete(e),i||t.get(e)?.onExit()))}}function o(){let e="hidden"===document.visibilityState;if(e!==i)for(let n of(i=e,r)){let r=t.get(n);e?r?.onExit():r?.onEnter()}}function s(){if(!i)for(let e of(i=!0,r))t.get(e)?.onExit()}e.s(["observeViewport",0,function(e,l){if("u"<typeof IntersectionObserver)return()=>{};let u=(n||(n=new IntersectionObserver(a),i="hidden"===document.visibilityState,document.addEventListener("visibilitychange",o),window.addEventListener("pagehide",s),window.addEventListener("pageshow",o)),n);return t.set(e,l),u.observe(e),()=>{u.unobserve(e),t.delete(e),r.delete(e)}}])},884214,e=>{"use strict";var t=e.i(389959),r=e.i(709485),n=e.i(415541),i=e.i(766057),a=e.i(892663);e.s(["useAutoLogView",0,function({elementId:e,trackViewport:o=!1}){let s=(0,t.useRef)(null),l=(0,t.useRef)(null);return(0,t.useCallback)(t=>{if(l.current?.(),l.current=null,!t)return;let u=(0,i.extractProductArea)(t),c=(t,i)=>{(0,n.trackV2)(r.eventsV2.ELEMENT_VIEWED,{element_id:e,view_type:t,time_in_viewport:i,product_area:u})};if(c("element",null),!o)return;let d=()=>{if(null===s.current)return;let e=performance.now()-s.current;s.current=null,c("viewport",e)},p=(0,a.observeViewport)(t,{onEnter:()=>{s.current=performance.now(),c("viewport",null)},onExit:d});l.current=()=>{p(),d()}},[e,o])}])},843036,e=>{"use strict";var t=e.i(351623),r=e.i(481963),n=e.i(344480);e.i(975473);let i={},a=t.gql`
    query UpgradeButton {
  currentUser {
    id
    ...UserPlanStateCurrentUser
  }
}
    ${r.UserPlanStateCurrentUserFragmentDoc}`;e.s(["useUpgradeButtonQuery",0,function(e){let t={...i,...e};return n.useQuery(a,t)}])},3466,e=>{"use strict";var t,r=e.i(276385),n=e.i(843036),i=e.i(712903),a=e.i(596139),o=e.i(615982),s=e.i(229375),l=e.i(532563);e.i(214847);var u=e.i(864300),c=e.i(415541),d=e.i(709485),p=e.i(911261),f=e.i(242917),m=e.i(643484),g=e.i(419635),h=e.i(488299),v=((t=v||{}).TrialUpgrade="trial_upgrade",t.Default="default",t);e.s(["default",0,({context:e,variant:t="outlined",onCancel:v,onClickCallback:y,text:_,content:T,surface:P,onPlanCheckoutComplete:E,iconButton:b,redirectPath:C,modalHeadingText:w,modalSubHeadingText:R,directCheckout:x=!1,planPeriod:I="monthly",hideCoreIcon:A,...S})=>{let O=(0,u.useIntl)(),{loading:U}=(()=>{let{data:e,loading:t}=(0,n.useUpgradeButtonQuery)();if(t)return{loading:!0,upgradeType:null};let r=e?.currentUser?(0,l.getCurrentPlanState)({user:e.currentUser}):null;return r?.plan.name===a.corePlanName&&null!==r.plan.trial?{loading:!1,upgradeType:"trial_upgrade"}:{loading:!1,upgradeType:"default"}})(),{show:k}=(0,f.useGlobalModal)(),{openCheckout:M,isLoading:F}=(0,p.useRegionalCheckout)(),D=(0,s.usePlanCheckoutUrl)({prefix:a.corePlanPrefix,interval:I,source:e,successRedirectPath:C,cancelRedirectPath:C}),L=_||O.formatMessage({id:"billing.upgradeButtonJoinCore",defaultMessage:"Join Replit {corePlanName}"},{corePlanName:a.corePlanName}),j=(0,o.getSubscriptionPlanChangeEntryPoint)(e),z=()=>{(0,c.track)(d.events.UPGRADE_SELECTED,{source:e}),y&&y()},N=e=>(0,o.startSubscriptionPlanChangeFlow)(j,{flow_type:"purchase_flow",element:"upgrade_button",flow_step_key:"started",interaction_status:"success",surface:e,...x?{target_interval:I}:{}}),H=async()=>{z();let t=N("checkout_modal");try{await k("MembershipPurchaseModal",{analyticsContext:{upgrade:{context:e,surface:P}},onPurchaseComplete:E,redirectPath:C,headingText:w,subHeadingText:R,subscriptionPurchaseFlow:t})}finally{v&&v()}};if(b)return(0,r.jsx)(h.IconButton,{alt:L,onClick:H,disabled:U,children:(0,r.jsx)(i.default,{})});if(x){let{className:e,clsx:n,disabled:s,slot:l,...u}=S;if(M){let l=U||F;return(0,r.jsx)(m.Button,{...u,iconLeft:A?void 0:(0,r.jsx)(i.default,{}),variant:t,clsx:[e,n,{loading:l,loaded:!l}],disabled:l||s,loading:l,onClick:()=>{z();let e=N("membership_plan_cards");(0,o.trackSubscriptionPurchaseSubmission)(e,{flow_type:"purchase_flow",element:"provider_launch_button",flow_step_key:"submitted",payment_provider:"razorpay",interaction_status:"success",surface:"membership_plan_cards",target_interval:I}),M({planPrefix:a.corePlanPrefix,planPeriod:I})},text:T??L})}return(0,r.jsx)(g.ButtonLink,{...u,iconLeft:A?void 0:(0,r.jsx)(i.default,{}),variant:t,clsx:[e,n,{loading:U,loaded:!U}],disabled:U||s,href:D,onClick:e=>{z();let t=N("membership_plan_cards");(0,o.trackSubscriptionPurchaseSubmission)(t,{flow_type:"purchase_flow",element:"provider_launch_button",flow_step_key:"submitted",payment_provider:"stripe",interaction_status:"success",surface:"membership_plan_cards",target_interval:I}),(0,o.redirectSubscriptionCheckoutClick)(e,D,t)},text:T??L})}return(0,r.jsx)(m.Button,{...S,iconLeft:A?void 0:(0,r.jsx)(i.default,{}),variant:t,clsx:[S.className,S.clsx,{loading:U,loaded:!U}],loading:U,onClick:H,text:T??L})}])},481963,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment TrialWillCancelAtCurrentUser on CurrentUser {
  id
  isSubscribed
  paymentMethod {
    __typename
    ... on PaymentMethod {
      id
      isSaved
    }
  }
  billingInfo {
    planInfo {
      cancelAt
    }
  }
  userSubscription {
    isTrial
    timeRemainingInTrial
  }
}
    `,n=t.gql`
    fragment UserPlanStateCurrentUser on CurrentUser {
  id
  ...TrialWillCancelAtCurrentUser
  userSubscriptionType
  billingInfo {
    planInfo {
      interval
      provider
    }
  }
  userSubscription {
    isTrial
  }
}
    ${r}`;e.s(["TrialWillCancelAtCurrentUserFragmentDoc",0,r,"UserPlanStateCurrentUserFragmentDoc",0,n])},532563,e=>{"use strict";var t=e.i(908796),r=e.i(596139),n=e.i(810461);let i=e=>{let{billingInfo:t,userSubscription:r,paymentMethod:n}=e,i=n?.__typename==="PaymentMethod"&&n.isSaved;if(!r?.isTrial)return null;let a=t?.planInfo?.cancelAt??(i?null:r?.timeRemainingInTrial??null);return a?new Date(a):null};e.s(["getCurrentPlanState",0,function({user:e}){let{userSubscriptionType:a,billingInfo:o,userSubscription:s}=e;if(null==s||null==a)return{showUpgradeCta:!0,plan:{name:r.freePlanName}};let l=i(e),u=a===t.UserSubscriptionTypeEnum.Pro?r.proPlanName:r.corePlanName,c=!1===s.isTrial,d=!0===s.isTrial&&null===l;return{showUpgradeCta:!c&&!d,plan:{name:u,period:(0,n.planPeriodFromInterval)(o?.planInfo?.interval),trial:s?.isTrial?{cancelsAt:l,isManuallyCancelled:(e=>{let{billingInfo:t}=e;return!!t?.planInfo?.cancelAt})(e)}:null,provider:o?.planInfo?.provider??t.PaymentProviderEnum.Stripe}}},"trialWillCancelAt",0,i])},229375,e=>{"use strict";var t=e.i(15801),r=e.i(568087),n=e.i(596139),i=e.i(561646),a=e.i(39182);e.s(["usePlanCheckoutUrl",0,function(e){let o=(0,t.useRouter)(),s=function(e,t){if(e.prefix===n.corePlanPrefix){let r=(0,n.getCheckoutablePriceOption)({...e,flags:t});if(!r)throw Error(`Core price not found for interval ${e.interval}`);return`stripe-checkout-by-price/${r.externalId}`}let r=(0,n.getCheckoutablePriceOption)({...e,flags:t});if(!r)throw Error(`Pro price not found for interval ${e.interval}, tier ${e.tier}`);return`stripe-checkout-by-price/${r.externalId}`}(e,(0,i.usePricingFlags)()),l=o.query[r.BONSAI_VERSION_QUERY_PARAM],u=Array.isArray(l)?l[0]:l;return(0,a.buildCheckoutUrl)({path:s,source:e.source,coupon:e.coupon,successRedirectPath:e.successRedirectPath,cancelRedirectPath:e.cancelRedirectPath,vBonsai:u})}])},946689,e=>{"use strict";var t=e.i(15801),r=e.i(389959),n=e.i(629107),i=e.i(568087),a=e.i(780902),o=e.i(753451);function s(e,t,r){let i=t?`/t/${encodeURIComponent(t)}/chats`:"/chats";return`${i}/${(0,n.conversationUrlKey)(e,r)}`}function l(){var e;let n=JSON.stringify((e=(0,t.useRouter)().query,Object.fromEntries(i.BONSAI_WEBVIEW_QUERY_PARAM_KEYS.flatMap(t=>{let r=e[t];return"string"==typeof r?[[t,r]]:[]}))));return(0,r.useMemo)(()=>JSON.parse(n),[n])}e.s(["conversationWorkspacePath",0,s,"useBonsaiWebviewQuery",0,l,"useConversationWorkspaceHref",0,function(){let e,n,u=(e=(0,t.useRouter)(),n=(0,a.useIsMobile)(),function({isMobileUserAgent:e,mobileWebview:t,tabletWorkspace:r}){return!r&&(t||e)}({isMobileUserAgent:n,mobileWebview:"true"===e.query.mobileWebview||"1"===e.query.mobileWebview,tabletWorkspace:"true"===e.query.tabletWorkspace||"1"===e.query.tabletWorkspace})),c=(0,a.useIsMobile)(),d=(0,o.useIsInBonsaiWebview)(),p=l();return(0,r.useCallback)((e,t,r)=>(function(e,t,r,n,a=!1,o){let l={...r,...!t&&o?.openSidebar?{sidebar:"open"}:{},...o?.nativeNewChat?{nativeNewChat:"1"}:{}},u=new URLSearchParams(l).toString();return{href:{pathname:t?"/chatMobile":"/replEnvironmentDesktop",query:{...l,conversationId:e,...n?{orgSlug:n}:{}}},as:`${s(e,n,o?.slug)}${u?`?${u}`:""}${a?i.NO_UNIVERSAL_LINKS_HASH:""}`}})(e,u,p,t,c&&!d,r),[d,c,u,p])}])},44477,e=>{"use strict";var t=e.i(908796);let r={admin:{name:"Admin",nameId:"clui.roleAdmin"},support:{name:"Support",nameId:"clui.roleSupport"},developer:{name:"Developer",nameId:"clui.roleDeveloper"},billing_admin:{name:"Billing admin",nameId:"clui.roleBillingAdmin"},sales:{name:"Sales",nameId:"clui.roleSales"},trust_and_safety:{name:"Trust & Safety",nameId:"clui.roleTrustAndSafety"}};e.s(["getAdminCluiRoleLabel",0,function(e,t){let{name:n,nameId:i}=r[t];return e.formatMessage({id:i,defaultMessage:n})},"getCurrentAdminCluiRoles",0,function(e){return(e??[]).flatMap(({key:e})=>{switch(e){case t.UserRoles.Admin:return["admin"];case t.UserRoles.Support:return["support"];case t.UserRoles.Developer:return["developer"];case t.UserRoles.BillingAdmin:return["billing_admin"];case t.UserRoles.Sales:return["sales"];case t.UserRoles.TrustAndSafety:return["trust_and_safety"];default:return[]}})}])},19715,e=>{"use strict";var t=e.i(407595),r=e.i(919073);e.s(["useSelectedRowClsx",0,function(e){let n=(0,r.useShadesSurface)({});return e?[n,t.selectionOutlineRuiClasses]:void 0}])},884033,e=>{"use strict";var t=e.i(351623),r=e.i(730029);let n=t.gql`
    fragment WorkspaceDropdownCustomer on Customer {
  id
  orgs {
    __typename
    ... on OrgConnection {
      items {
        id
        name
        type
        slug
        currentUserRole
        image
        dealContext {
          dealType
          salesContactEmail
        }
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
  }
  authorizations {
    createWorkspace {
      isAuthorized
      code
    }
  }
}
    `,i=t.gql`
    fragment WorkspaceDropdownCurrentUser on CurrentUser {
  id
  image
  username
  url
  fullName
  firstName
  email
  isMemberOfAnyOrg
  customer {
    ...WorkspaceDropdownCustomer
  }
  customers {
    items {
      name
      ...WorkspaceDropdownCustomer
    }
  }
  ...CoreSubscriptionPlanStatus
}
    ${n}
${r.CoreSubscriptionPlanStatusFragmentDoc}`;e.s(["WorkspaceDropdownCurrentUserFragmentDoc",0,i])},127384,e=>{"use strict";e.s(["APP_HEADER_HEIGHT",0,40,"COLLAPSED_TASK_SIDEBAR_WIDTH",0,44,"GLOBAL_SIDEBAR_INSET_DATA_ATTRIBUTE",0,"data-awaiting-global-sidebar-inset","GLOBAL_SIDEBAR_RESIZE_OVERLAY_Z_INDEX",0,1031,"HEADER_HEIGHT",0,48,"HEADER_Z_INDEX",0,1002,"MAX_GLOBAL_SIDEBAR_WIDTH_PERCENTAGE",0,.3,"MAX_MOBILE_GLOBAL_SIDEBAR_WIDTH",0,360,"MIN_GLOBAL_SIDEBAR_WIDTH",0,200,"MOBILE_GLOBAL_SIDEBAR_VERTICAL_INSET",0,5,"MOBILE_GLOBAL_SIDEBAR_WIDTH_PERCENTAGE",0,.85,"SIDEBAR_OVERLAY_Z_INDEX",0,1e3,"SIDEBAR_WIDTH",0,240,"SIDEBAR_Z_INDEX",0,1001])},39182,e=>{"use strict";var t=e.i(568087);e.s(["buildCheckoutUrl",0,function({path:e,coupon:r,source:n,successRedirectPath:i,cancelRedirectPath:a,vBonsai:o}){let s=new URLSearchParams;r&&s.set("coupon",r),n&&s.set("source",n),i&&s.set("successRedirectPath",i),a&&s.set("cancelRedirectPath",a),o&&s.set(t.BONSAI_VERSION_QUERY_PARAM,o);let l=s.toString();return`/${e}${l?`?${l}`:""}`},"getCheckoutPageHref",0,function(e){return`/checkout?source=${encodeURIComponent(e)}`}])},61041,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let n={},i=t.gql`
    fragment UniversalSettingsNavigationCustomerAuthorizations on Customer {
  id
  authorizations {
    viewSubscription {
      isAuthorized
      message
    }
    viewSeats {
      isAuthorized
      message
    }
    viewUsage {
      isAuthorized
      message
    }
    editSettings {
      isAuthorized
      message
    }
    enablePromotions {
      isAuthorized
      message
    }
    manageApiKeys @include(if: $includeOrg) {
      isAuthorized
    }
  }
}
    `,a=t.gql`
    fragment UniversalSettingsNavigationUsagePageAuthorizations on UsagePageAuthorizations {
  usageBreakdownEnabled
  viewPage {
    isAuthorized
    message
  }
}
    `,o=t.gql`
    fragment UniversalSettingsNavigationOrgAuthorizations on OrgAuthorizations {
  viewUsage {
    isAuthorized
    message
  }
  viewOrgMetadata {
    isAuthorized
    message
  }
  viewOrgGroups {
    isAuthorized
  }
  viewOrgCustomInstructions {
    isAuthorized
    message
  }
  viewPersonalMemory {
    isAuthorized
    message
  }
  viewOrgCustomSkills {
    isAuthorized
    message
  }
  viewArtifactTemplates {
    isAuthorized
    message
  }
  viewOrgSecurity {
    isAuthorized
    message
  }
  viewOwnBudgets {
    isAuthorized
    message
  }
}
    `,s=t.gql`
    query PrefetchUniversalSettingsNavigation($orgId: String!, $orgSlug: String!, $includeOrg: Boolean!) {
  currentUser {
    __typename
    ... on CurrentUser {
      id
      username
      image
      personalOrgAuthorizations @skip(if: $includeOrg) {
        ...UniversalSettingsNavigationOrgAuthorizations
      }
      existingCustomer @skip(if: $includeOrg) {
        __typename
        id
        usagePageAuthorizations {
          ...UniversalSettingsNavigationUsagePageAuthorizations
        }
        ...UniversalSettingsNavigationCustomerAuthorizations
      }
    }
  }
  getOrg(orgSlug: $orgSlug) @include(if: $includeOrg) {
    __typename
    ... on Org {
      id
      slug
      image
      name
      authorizations {
        ...UniversalSettingsNavigationOrgAuthorizations
      }
      customer {
        ... on Customer {
          __typename
          id
          pageAuthorizations: usagePageAuthorizations(contextOrgId: $orgId) {
            ...UniversalSettingsNavigationUsagePageAuthorizations
          }
          ...UniversalSettingsNavigationCustomerAuthorizations
        }
      }
    }
    ... on NotFoundError {
      message
    }
  }
}
    ${o}
${a}
${i}`,l=t.gql`
    query PrefetchUniversalSettingsDedicatedAccess($customerId: Int!, $includeApprovalRequests: Boolean!) {
  getCustomer(customerId: $customerId) {
    __typename
    ... on Customer {
      id
      authorizations {
        editAuthSessionExpiry {
          isAuthorized
        }
        manageApprovalRequests @include(if: $includeApprovalRequests) {
          isAuthorized
        }
      }
    }
  }
}
    `,u=t.gql`
    query UniversalSettingsNavigationCustomerReference($customerId: Int!) {
  getCustomer(customerId: $customerId) {
    __typename
    ... on Customer {
      id
    }
  }
}
    `,c=t.gql`
    query UniversalSettingsNavigationCurrentUserCustomerReference {
  currentUser {
    __typename
    ... on CurrentUser {
      id
      customer {
        __typename
        id
      }
    }
  }
}
    `,d=t.gql`
    query UniversalSettingsNavigationOrgReference($orgId: String!) {
  getOrg(orgId: $orgId) {
    __typename
    ... on Org {
      id
    }
  }
}
    `;e.s(["UniversalSettingsNavigationCurrentUserCustomerReferenceDocument",0,c,"UniversalSettingsNavigationCustomerReferenceDocument",0,u,"UniversalSettingsNavigationOrgReferenceDocument",0,d,"usePrefetchUniversalSettingsDedicatedAccessQuery",0,function(e){let t={...n,...e};return r.useQuery(l,t)},"usePrefetchUniversalSettingsNavigationQuery",0,function(e){let t={...n,...e};return r.useQuery(s,t)}])},587719,e=>{"use strict";var t=e.i(389959),r=e.i(61041);e.s(["usePrefetchUniversalSettingsNavigation",0,function(e){let n="page"===e.type?e.scope:void 0,i=n?.type==="org"?n.orgId:void 0,a=n?.type==="org"?n.orgSlug:void 0,[o,s]=(0,t.useState)(null),l=(0,t.useRef)(null),u=null;n?.type==="org"?u=`org:${n.orgId}`:n?.type==="personal"&&(u="personal");let c=void 0!==i&&void 0!==a,d=null!==u&&o!==u;(0,t.useEffect)(()=>{if(!d)return;let e=()=>s(u);if("function"==typeof window.requestIdleCallback){let t=window.requestIdleCallback(e,{timeout:2e3});return()=>window.cancelIdleCallback(t)}let t=window.setTimeout(e,0);return()=>window.clearTimeout(t)},[u,d]);let p=null!==u&&o===u,{data:f,client:m}=(0,r.usePrefetchUniversalSettingsNavigationQuery)({variables:{orgId:i??"",orgSlug:a??"",includeOrg:c},skip:!p,ssr:!1,fetchPolicy:"cache-first"}),g=f?.currentUser?.__typename==="CurrentUser"?f.currentUser:void 0,h=c&&f?.getOrg?.__typename==="Org"&&f.getOrg.id===i&&f.getOrg.slug===a?f.getOrg:void 0,v=h?.customer?.__typename==="Customer"?h.customer:void 0,y=g?.existingCustomer?.__typename==="Customer"?g.existingCustomer:void 0,_=c?v:y,T=p&&_?.authorizations.editSettings.isAuthorized===!1;(0,r.usePrefetchUniversalSettingsDedicatedAccessQuery)({variables:{customerId:_?.id??0,includeApprovalRequests:c},skip:!T,ssr:!1,fetchPolicy:"cache-first"}),(0,t.useEffect)(()=>{p&&void 0!==f&&l.current!==u&&void 0!==_&&(m.cache.writeQuery({query:r.UniversalSettingsNavigationCustomerReferenceDocument,variables:{customerId:_.id},data:{getCustomer:{__typename:"Customer",id:_.id}}}),c||void 0===g||m.cache.writeQuery({query:r.UniversalSettingsNavigationCurrentUserCustomerReferenceDocument,data:{currentUser:{__typename:"CurrentUser",id:g.id,customer:{__typename:"Customer",id:_.id}}}}),void 0!==h&&m.cache.writeQuery({query:r.UniversalSettingsNavigationOrgReferenceDocument,variables:{orgId:h.id},data:{getOrg:{__typename:"Org",id:h.id}}}),l.current=u)},[m.cache,g,_,u,f,c,h,p])}])},742881,e=>{"use strict";var t=e.i(598986);function r(e){return`remix-gallery:autostart:${e}`}e.s(["consumeRemixGalleryAutostart",0,function(e){let n=r(e),i=t.default.get(n,"object");return null===i?null:(t.default.remove(n),{autostart:!0===i.autostart,canvasMode:!0===i.canvasMode})},"markRemixGalleryAutostart",0,function(e,n){t.default.set(r(e),n)}])},289884,e=>{"use strict";var t=e.i(389959);e.s(["default",0,function(e,r,{includeKeyboardActivation:n=!1,keyboardInsideElement:i=null}={}){let a=(0,t.useRef)(null),o=(0,t.useRef)(i);o.current=i;let s=(0,t.useCallback)(t=>{a.current?.contains(t.target)||e(t)},r),l=(0,t.useCallback)(e=>{"Enter"!==e.key&&" "!==e.key||o.current?.contains(e.target)||s(e)},[s]);return(0,t.useEffect)(()=>(document.addEventListener("pointerdown",s,!0),n&&document.addEventListener("keydown",l,!0),()=>{document.removeEventListener("pointerdown",s,!0),n&&document.removeEventListener("keydown",l,!0)}),[n,s,l]),a}])},714562,e=>{"use strict";var t=e.i(351623),r=e.i(42585),n=e.i(299020);let i={},a=t.gql`
    mutation ForkReplCreateRepl($input: CreateReplInput!, $isTitleAutoGenerated: Boolean) {
  createRepl(input: $input, isTitleAutoGenerated: $isTitleAutoGenerated) {
    ... on Repl {
      ...ReplsListRepl
      id
      org {
        id
      }
      url
      isPrivate
      language
      nextPagePathname
      config {
        isAgentRepl
      }
      origin {
        id
        isOwner
      }
      source {
        release {
          id
          repl {
            id
            title
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
        }
      }
    }
    ... on UserError {
      message
    }
  }
}
    ${r.ReplsListReplFragmentDoc}`;e.s(["useForkReplCreateReplMutation",0,function(e){let t={...i,...e};return n.useMutation(a,t)}])},738522,e=>{"use strict";var t=e.i(15801),r=e.i(389959),n=e.i(714562),i=e.i(871203),a=e.i(570438),o=e.i(151027),s=e.i(320216),l=e.i(429843),u=e.i(370511),c=e.i(415541),d=e.i(709485),p=e.i(540742),f=e.i(758423),m=e.i(921125);e.s(["default",0,function({onFork:e,onError:g,onNavigationComplete:h,replLinkOptions:v}={}){let y=(0,t.useRouter)(),_=(0,a.useCurrentUserId)(),{showError:T}=(0,s.default)(),P=(0,p.default)(),[E,b]=(0,n.useForkReplCreateReplMutation)({update:(e,{data:t})=>{let r=t?.createRepl.__typename==="Repl"?t.createRepl:null;if(null!==r&&null!==_)try{(0,f.insertReplIntoSidebarList)({cache:e,userId:_,repl:r})}catch(e){l.logger.error("failed to add a remixed repl to the sidebar",{replid:r.id,reason:e instanceof Error?e.message:String(e)})}},onCompleted:async t=>{let r=t?.createRepl.__typename==="Repl"?t.createRepl:null;if(!r)return;if(e)try{await e(t)}catch(e){l.logger.error("fork onFork callback failed",e instanceof Error?e:Error(String(e)))}let n=(0,i.shouldMigrateReplToNix)(r);if("/replEnvironmentDesktop"===y.pathname||"/replEnvironmentMobile"===y.pathname||"/replView"===y.pathname&&n){window.location.href=(0,m.replLinkFullUrl)(r,v);return}let a=(0,m.replLinkProps)(r,v);y.push({...a.href,pathname:P},a.as).then(e=>{e&&(b.reset?.(),h?.())})}}),C=b.data?.createRepl.__typename==="UserError"?b.data?.createRepl.message:b.error?.message;return(0,r.useEffect)(()=>{C&&(T(C??"Failed to fork repl"),g&&g())},[C,T,g]),[(0,r.useCallback)(({originId:e,replReleaseId:t,title:r,description:n,isPrivate:i,folderId:a,trackingData:s,forkToPersonal:l,orgId:p,connectionIds:f,zealot:m,isWebDesignMockup:g,copyDatabase:h,isTitleAutoGenerated:v})=>{(0,c.track)(d.events.FORK_REQUESTED,s),E({variables:{isTitleAutoGenerated:v,input:{originId:e,replReleaseId:t,title:r,isPrivate:i,description:n,folderId:a,forkToPersonal:l,gitRemoteUrl:"",orgId:p,connectionIds:f,zealot:m,isWebDesignMockup:g,copyDatabase:h}}}).then(r=>{let n=r.data?.createRepl.__typename==="Repl"?r.data.createRepl:null;n&&(0,c.track)(d.events.REPL_CREATED,{isSignup:!1,isOnboarding:!1,...s,isForked:!0,isPrivate:n.isPrivate,replId:n.id,isRenamed:!1,language:n.language,isSelfForked:!!n.origin?.isOwner,isTemplateFork:!!t,originId:e,templateReplId:n.source?.release?.repl?.id||void 0,templateTitle:n.source?.release?.repl?.title,templateOwner:n.source?.release?.repl?.owner?.username,templateType:n.source?.release?.repl?(0,u.getTemplateTrackingType)(n.source.release.repl):void 0,orgContext:(0,o.getOrgTrackingContext)(n.org?{id:n.org.id}:void 0)})})},[E]),{loading:!!b.loading||!!b.data&&"Repl"===b.data.createRepl.__typename}]}])},395108,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let n={},i=t.gql`
    fragment PersonalWorkspacesDisabledCurrentUser on CurrentUser {
  id
  personalWorkspacesDisabled
}
    `,a=t.gql`
    query PersonalWorkspacesDisabled {
  currentUser {
    ...PersonalWorkspacesDisabledCurrentUser
  }
}
    ${i}`;e.s(["usePersonalWorkspacesDisabledQuery",0,function(e){let t={...n,...e};return r.useQuery(a,t)}])},294827,e=>{"use strict";var t=e.i(908796),r=e.i(395108);e.s(["usePersonalWorkspacesDisabled",0,function(){let{data:e}=(0,r.usePersonalWorkspacesDisabledQuery)(),n=e?.currentUser,i=n?.personalWorkspacesDisabled??t.PersonalWorkspacesDisabledMode.None,a=i!==t.PersonalWorkspacesDisabledMode.None;return{shouldHidePersonalWorkspace:i===t.PersonalWorkspacesDisabledMode.Personal||i===t.PersonalWorkspacesDisabledMode.Full,restrictionMode:i,isRestrictedDomain:a}}])},473072,e=>{"use strict";var t=e.i(389959),r=e.i(796424);e.s(["default",0,function(){let e=(0,t.useContext)(r.default);if(!e)throw Error("Expected Repl ID to be in context");return e}])},730029,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment CoreSubscriptionPlanStatus on CurrentUser {
  hasCore: subscriptionIsType(subscriptionType: HACKER_PRO)
}
    `;e.s(["CoreSubscriptionPlanStatusFragmentDoc",0,r])},797265,e=>{"use strict";var t=e.i(800686);let r=(0,t.defineMessages)({singular:{id:"home.replNounSingular",defaultMessage:"Project"},plural:{id:"home.replNounPlural",defaultMessage:"Projects"},singularLower:{id:"home.replNounSingularLower",defaultMessage:"project"},pluralLower:{id:"home.replNounPluralLower",defaultMessage:"projects"}}),n=(0,t.defineMessages)({singular:{id:"home.chatNounSingular",defaultMessage:"Chat"},plural:{id:"home.chatNounPlural",defaultMessage:"Chats"},singularLower:{id:"home.chatNounSingularLower",defaultMessage:"chat"},pluralLower:{id:"home.chatNounPluralLower",defaultMessage:"chats"}});e.s(["CHAT_DISPLAY_NAME",0,n,"REPL_DISPLAY_NAME",0,r])},846128,e=>{"use strict";function t(e,t){return null!==n(e,t)}function r(e,t,r,n,i){let a=n.indexOf(r);if(-1!==a){let t=e**e;return{ranges:[{from:a,to:a+r.length}],score:0===a?1.5*t:t}}let o=r.length,s=[],l=0,u=0,c=-2;for(let e=0;e<o;e++){let i=n.indexOf(r[e],c+1);if(-1===i||i>=t)return null;i===c+1?(u+=1+u,s[s.length-1].to+=1):(u=1,s.push({from:i,to:i+1})),l+=u,c=i}if(i){let t=e<2?null:2===e?3:3===e?6:4===e?10:20;if(null!==t&&l<t)return null}return{ranges:s,score:l}}function n(e,t,n){let i=n?.caseSensitive??!1,a=i?t:t.toLowerCase(),o=i?e:e.toLowerCase();return r(e.length,t.length,o,a,n?.filterLowMatchScores??!1)}e.s(["default",0,{filter:function(e,t,n){if(0===t.length)return[];let i=n||{},a=e.toLowerCase(),o=[];return t.forEach((t,n)=>{let s="string"==typeof t?t:i.extract?.(t);if(void 0===s)return;let l=r(e.length,s.length,a,s.toLowerCase(),!1);null!==l&&o.push({score:l.score,index:n,original:t})}),o.sort(function(e,t){let r=t.score-e.score;return r||e.index-t.index}),o},test:t,match:n,simpleFilter:function(e,r){return r.filter(function(r){return t(e,r)})}}])},544952,e=>{"use strict";var t=e.i(415756);function r({queryLength:e}){return e<2?null:2===e?44:3===e?68:4===e?82:16*e}function n(e){let t=[...e.values()].sort((e,t)=>e-t),r=[],n=t[0],i=t[0];for(let e=1;e<t.length;e++)t[e]===i+1?i++:(r.push({from:n,to:i+1}),n=t[e],i=t[e]);return r.push({from:n,to:i+1}),r}e.s(["fzfMatch",0,function(e,i,{filterLowMatchScores:a,...o}={}){let s=new t.Fzf([i],o).find(e);if(0===s.length)return null;let l=s[0];if(a){let t=r({queryLength:e.length});if(t&&l.score<t)return null}return{score:l.score,ranges:n(l.positions)}},"getMinMatchScoreForQuery",0,r,"indiciesToRanges",0,n])},14976,e=>{"use strict";e.s(["measureMouseMove",0,function(e,t,r={}){let n=void 0===r.moveThreshold;e.preventDefault();let{pageX:i,pageY:a,pointerId:o,nativeEvent:s}=e,l={x:i,y:a},u={x:0,y:0},c=e=>"pointerId"in e&&e.pointerId!==o;t.start&&t.start({mouse:l,shiftKey:e.shiftKey});let d=e=>{let{pageX:i,pageY:a}=e,o={x:i,y:a};if(u={x:o.x-l.x,y:o.y-l.y},void 0!==r.moveThreshold&&!n){if(Math.abs(u.x)<r.moveThreshold&&Math.abs(u.y)<r.moveThreshold)return;n=!0}t.move({delta:u,shiftKey:e.shiftKey,mouse:o})},p=null,f=null,m=e=>{c(e)||(e.preventDefault(),f=e,null!==p&&cancelAnimationFrame(p),p=requestAnimationFrame(()=>{p=null;let e=f;f=null,e&&d(e)}))},g=({flushPendingMove:i})=>{document.removeEventListener("pointerdown",v,{capture:!0}),document.removeEventListener("pointermove",m),document.removeEventListener("pointerup",h),document.removeEventListener("pointercancel",h),document.removeEventListener("touchcancel",h),window.removeEventListener("blur",h),null!==p&&(cancelAnimationFrame(p),p=null);let a=f;f=null;try{i&&a&&d(a)}finally{(void 0===r.moveThreshold||n)&&t.end&&t.end({delta:u,shiftKey:e.shiftKey})}},h=e=>{c(e)||g({flushPendingMove:!0})},v=e=>{c(e)||e===s||g({flushPendingMove:!1})};document.addEventListener("pointerdown",v,{capture:!0}),document.addEventListener("pointermove",m),document.addEventListener("pointerup",h),document.addEventListener("pointercancel",h),document.addEventListener("touchcancel",h),window.addEventListener("blur",h)}])},810461,e=>{"use strict";e.s(["planPeriodFromInterval",0,e=>"month"===e?"monthly":"year"===e?"yearly":null])},982728,e=>{"use strict";function t(){if(document.getElementById("razorpay-color-scheme-fix"))return;let e=document.createElement("style");e.id="razorpay-color-scheme-fix",e.textContent='iframe[src*="razorpay"] { color-scheme: light; }\n#rzp-sdk-root, .rzp-sdk-root { color-scheme: light; }',document.head.appendChild(e)}let r=new Map;function n(e){let t=r.get(e);if(t)return t;document.querySelector(`script[src="${e}"]`)?.remove();let n=new Promise((t,n)=>{let i=document.createElement("script");i.src=e,i.onload=()=>{r.delete(e),t()},i.onerror=()=>{r.delete(e),n(Error(`Failed to load script: ${e}`))},document.head.appendChild(i)});return r.set(e,n),n}async function i(){t(),window.RZPCrossBorderPrePay||(await n("https://checkout.razorpay.com/v1/checkout.js"),await n("https://cross-border-cdn.razorpay.com/custom-pre-payment-module/build/browser/rzp-xb-pre-pay-module.min.js"))}e.s(["MERCHANT_IMAGE",0,"https://replit.com/public/images/replit-logo.png","MERCHANT_NAME",0,"Replit","MERCHANT_THEME",0,{color:"#232430"},"injectRazorpayColorSchemeFix",0,t,"loadRazorpayScript",0,i,"loadScript",0,n])},370511,e=>{"use strict";var t,r=e.i(761201),n=((t=n||{}).Community="community",t.Official="official",t);e.s(["getTemplateTrackingType",0,function(e){return e.owner?.username===r.OFFICIAL_TEMPLATE_USERNAME?"official":"community"}])},343911,e=>{"use strict";var t=e.i(351623),r=e.i(299020);let n={},i=t.gql`
    mutation ConfirmRazorpayCheckout($input: ConfirmRazorpayCheckoutInput!) {
  confirmRazorpayCheckout(input: $input)
}
    `,a=t.gql`
    mutation CreateReplitPlanCheckoutSessionForRazorpay($input: CreateReplitPlanCheckoutSessionInput!) {
  createReplitPlanCheckoutSession(input: $input) {
    ... on RazorpayCheckoutSessionResult {
      checkoutToken
      keyId
      checkoutSessionId
      currency
      prefillName
      prefillEmail
      amount
    }
    ... on UserError {
      message
    }
    ... on UnauthorizedError {
      message
    }
    ... on TooManyRequestsError {
      message
    }
  }
}
    `;e.s(["useConfirmRazorpayCheckoutMutation",0,function(e){let t={...n,...e};return r.useMutation(i,t)},"useCreateReplitPlanCheckoutSessionForRazorpayMutation",0,function(e){let t={...n,...e};return r.useMutation(a,t)}])},452572,e=>{"use strict";var t=e.i(15801),r=e.i(389959),n=e.i(343911),i=e.i(320216),a=e.i(982728);e.s(["useRazorpayCheckout",0,function({onSuccess:e,onBeforeOpen:o,onDismiss:s}={}){let[l,u]=(0,r.useState)(!1),{showError:c}=(0,i.default)(),d=(0,t.useRouter)(),[p]=(0,n.useCreateReplitPlanCheckoutSessionForRazorpayMutation)(),[f]=(0,n.useConfirmRazorpayCheckoutMutation)();return{openCheckout:(0,r.useCallback)(async({planPrefix:t,planPeriod:r,promoCodeExternalId:n,priceExternalId:i})=>{u(!0);let l=!1;try{let{data:u}=await p({variables:{input:{planPrefix:t,planPeriod:r,promoCodeExternalId:n,priceExternalId:i}}}),m=u?.createReplitPlanCheckoutSession;if(m?.__typename!=="RazorpayCheckoutSessionResult"||!m.keyId||!m.checkoutToken||null==m.amount||!m.currency)return void c("Unable to start checkout. Please try again.");if(await (0,a.loadRazorpayScript)(),!window.RZPCrossBorderPrePay)return void c("Unable to load payment provider. Please try again.");o&&(o(),l=!0,await new Promise(e=>setTimeout(e,250))),new window.RZPCrossBorderPrePay({key:m.keyId,amount:m.amount,currency:m.currency,name:a.MERCHANT_NAME,image:a.MERCHANT_IMAGE,theme:a.MERCHANT_THEME,checkout_session_id:m.checkoutToken,prefill:{name:m.prefillName??void 0,email:m.prefillEmail??void 0}},{onPaymentEvent:t=>{if("payment.success"!==t.event)return;let r=t.payment,n=t.razorpay_payment_id??r?.razorpay_payment_id;n&&m.checkoutSessionId&&f({variables:{input:{checkoutSessionId:m.checkoutSessionId,razorpayPaymentId:n}}}).catch(()=>{}),e?.(),d.push(`/stripe-checkout-success?sessionId=${m.checkoutSessionId}`)},onError:()=>{c("Payment failed. Please try again."),s?.()},onDismiss:()=>{s?.()}}).open()}catch{c("Checkout failed. Please try again."),l&&s?.()}finally{u(!1)}},[f,p,o,s,e,d,c]),isLoading:l}}])},911261,e=>{"use strict";var t=e.i(972152),r=e.i(452572),n=e.i(326523);e.s(["useRegionalCheckout",0,function({onSuccess:e,onBeforeOpen:i,onDismiss:a}={}){let o=(0,n.useRegionalPaymentProvider)(),{openCheckout:s,isLoading:l}=(0,r.useRazorpayCheckout)({onSuccess:e,onBeforeOpen:i,onDismiss:a});return o===t.CHECKOUT_PAYMENT_PROVIDER_RAZORPAY?{openCheckout:s,isLoading:l,provider:o}:{openCheckout:null,isLoading:!1,provider:o}}])},235826,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let n={},i=t.gql`
    query RegionalPaymentProviderCountry {
  country
}
    `;e.s(["useRegionalPaymentProviderCountryQuery",0,function(e){let t={...n,...e};return r.useQuery(i,t)}])},326523,e=>{"use strict";var t=e.i(389959),r=e.i(235826),n=e.i(972152),i=e.i(776065),a=e.i(933302);e.s(["useRegionalPaymentProvider",0,function(){let e=(0,a.useFeatureGate)("flag-razorpay-checkout",!1),o=(0,i.useQueryParam)("country","string"),{data:s,refetch:l}=(0,r.useRegionalPaymentProviderCountryQuery)({skip:!e}),u=(0,t.useRef)(null);if((0,t.useEffect)(()=>{e&&u.current!==o&&(u.current=o,l())},[o,e,l]),!e)return null;let c=s?.country??"",d=n.COUNTRY_PROVIDER_MAP[c]??null;return null===d?null:d},"useUserCountry",0,function(){let{data:e}=(0,r.useRegionalPaymentProviderCountryQuery)({ssr:!1,fetchPolicy:"cache-and-network"});return e?.country??null}])},998573,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var n=e.i(846545);let i={},a=t.gql`
    query notificationCount {
  currentUser {
    id
    notificationCount
  }
}
    `,o=t.gql`
    subscription notificationCountChanges {
  notificationCount
}
    `;e.s(["NotificationCountDocument",0,a,"useNotificationCountChangesSubscription",0,function(e){let t={...i,...e};return n.useSubscription(o,t)},"useNotificationCountQuery",0,function(e){let t={...i,...e};return r.useQuery(a,t)}])},980224,e=>{"use strict";var t=e.i(389959),r=e.i(998573);e.s(["default",0,function({skip:e=!1}={}){let{data:n,refetch:i,client:a}=(0,r.useNotificationCountQuery)({skip:e}),o=(0,t.useCallback)(e=>{n?.currentUser&&a.writeQuery({query:r.NotificationCountDocument,data:{...n,currentUser:{...n.currentUser,notificationCount:e}}})},[n?.currentUser?.id]);return(0,r.useNotificationCountChangesSubscription)({skip:e,onData:({data:{data:e}})=>{"number"==typeof e?.notificationCount&&o(e.notificationCount)}}),{count:Math.max(0,n?.currentUser?.notificationCount||0),refetch:i,setUnreadCount:o}}])},832753,e=>{e.v({aperture:"Overlay-module__--ci5a__aperture",noAnimation:"Overlay-module__--ci5a__noAnimation",transparent:"Overlay-module__--ci5a__transparent"})},451499,e=>{"use strict";var t=e.i(276385),r=e.i(971131),n=e.i(50696),i=e.i(691636),a=e.i(832753);let o=i.DefaultModalZIndex-1;function s({side:e,rect:t,docWidth:r,docHeight:n}){switch(e){case"left-top":return{top:0,left:0,width:Math.max(0,t.left),height:Math.max(0,t.bottom)};case"top-right":return{top:0,right:0,width:`calc(${r}px - ${Math.max(0,t.left)}px)`,height:Math.max(0,t.top)};case"right-bottom":return{bottom:0,right:0,width:`calc(${r}px - ${Math.max(0,t.right)}px)`,height:`calc(${n}px - ${Math.max(0,t.top)}px)`};case"bottom-left":return{bottom:0,left:0,width:Math.max(0,t.right),height:`calc(${n}px - ${Math.max(0,t.bottom)}px)`}}}e.s(["TourApertureOverlay",0,function({targetElement:e,transparent:i,disableAnimation:l}){let u=(0,n.useElementDomInfo)(e);if(null==u||null==e)return null;let c=[a.default.aperture,{[a.default.transparent]:i,[a.default.noAnimation]:l}],d={zIndex:o};return(0,r.createPortal)((0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)("div",{id:"wat",clsx:c,style:{...d,...s({side:"left-top",...u})}}),(0,t.jsx)("div",{clsx:c,style:{...d,...s({side:"top-right",...u})}}),(0,t.jsx)("div",{clsx:c,style:{...d,...s({side:"right-bottom",...u})}}),(0,t.jsx)("div",{clsx:c,style:{...d,...s({side:"bottom-left",...u})}})]}),document.body)},"TourApertureOverlayZIndex",0,o])},994078,e=>{"use strict";let t=e.i(451499).TourApertureOverlayZIndex+1;e.s(["TourOverlayWindowZIndex",0,t])},124619,e=>{e.v({popperHiddenWhenReferenceHidden:"TourPopover-module__rN5JsW__popperHiddenWhenReferenceHidden"})},270847,e=>{"use strict";var t=e.i(276385),r=e.i(389959),n=e.i(971131),i=e.i(32988),a=e.i(289884),o=e.i(994078),s=e.i(89148),l=e.i(919073),u=e.i(147622),c=e.i(884036),d=e.i(124619);e.s(["TourPopover",0,function({targetElement:e,activeStep:p,goto:f,currentStepIndex:m,totalSteps:g,done:h,onInteraction:v,disableAnimation:y,placement:_="right",offset:T=[0,16],cypressCloseData:P="tour-popover-close-button",onClickOutside:E,dismissOnKeyboardActivation:b=!1,innerRef:C,width:w,colorway:R="primary",doneText:x,doneButtonColorway:I,showSingleStepDone:A,styleVariant:S,hideIfPopperReferenceHidden:O=!0,dataAnalyticsId:U,boundaryElement:k="clippingParents",portalContainer:M=document.body,zIndex:F=o.TourOverlayWindowZIndex+1}){let[D,L]=(0,r.useState)(null),[j,z]=(0,r.useState)(null),[N,H]=(0,r.useState)(0),V=p.colorway??R,B=!1===V,W=(0,r.useMemo)(()=>e?{contextElement:e,getBoundingClientRect:()=>e.getBoundingClientRect(),positionVersion:N}:null,[e,N]),$=(0,i.usePopper)(W,D,{placement:_,modifiers:[{name:"arrow",options:{element:j,padding:8}},{name:"offset",options:{offset:T}},{name:"preventOverflow",options:{boundary:k,padding:8}},{name:"flip",enabled:!p.disablePlacementFlip,options:{boundary:k,fallbackPlacements:["top","bottom","left","right"]}},{name:"computeStyles",options:{adaptive:!1}}]}),q=$.update,G=(0,r.useCallback)(()=>{q?.()},[q]);(0,r.useEffect)(()=>{if(!D||"u"<typeof ResizeObserver)return;let e=new ResizeObserver(()=>{q?.()});return e.observe(D),()=>e.disconnect()},[D,q]),(0,r.useEffect)(()=>{if(!e)return;let t=e.getBoundingClientRect();H(e=>e+1);let r=window.setInterval(()=>{let r=e.getBoundingClientRect();(r.left!==t.left||r.top!==t.top||r.width!==t.width||r.height!==t.height)&&(t=r,H(e=>e+1))},500);return()=>window.clearInterval(r)},[e]),(0,r.useImperativeHandle)(C,()=>({updatePosition(){G()}}));let Y=(0,a.default)(()=>{E&&E()},[E],{includeKeyboardActivation:b,keyboardInsideElement:e}),Q={...$.styles.popper,zIndex:F,width:w??320,transition:y?"all":"all 0.5s ease-out"},X=O?d.default.popperHiddenWhenReferenceHidden:void 0;return B?(0,n.createPortal)((0,t.jsxs)(l.ShadesSurface,{innerRef:e=>{Y.current=e,L(e)},style:{...Q,boxShadow:s.tokens.shadow1},colorShade:"themePopup",elevate:"1x",border:"strong",br:8,clsx:X,"data-analytics-id":U,...$.attributes.popper,children:[(0,t.jsx)(c.Tour,{title:p.title,subtitle:p.subtitle,icon:p.icon,content:p.content,onDismiss:h,onInteraction:v,currentStepIndex:m,onCurrentStepChange:f,totalSteps:g,cypressCloseData:P,hideCloseButton:p.hideCloseButton,hideNavigation:p.hideNavigation,showSingleStepDone:A,styleVariant:S,colorway:V,doneText:x,doneButtonColorway:I,appearance:"plain"}),(0,t.jsx)(u.PopperArrow,{ref:z,style:$.styles.arrow,backgroundColor:l.SHADES_SURFACE_TOKENS_DO_NOT_USE.surfaceColor,borderColor:l.SHADES_SURFACE_TOKENS_DO_NOT_USE.borderColorStrong,zIndex:-1})]}),M):(0,n.createPortal)((0,t.jsxs)("div",{ref:e=>{Y.current=e,L(e)},style:Q,clsx:X,"data-analytics-id":U,...$.attributes.popper,children:[(0,t.jsx)(c.Tour,{title:p.title,subtitle:p.subtitle,icon:p.icon,content:p.content,onDismiss:h,onInteraction:v,currentStepIndex:m,onCurrentStepChange:f,totalSteps:g,cypressCloseData:P,hideCloseButton:p.hideCloseButton,hideNavigation:p.hideNavigation,showSingleStepDone:A,styleVariant:S,colorway:V,doneText:x,doneButtonColorway:I}),(0,t.jsx)(u.PopperArrow,{ref:z,style:$.styles.arrow,backgroundColor:V?s.colormap[V].dimmest:s.tokens.backgroundDefault,borderColor:V?s.colormap[V].dimmer:s.tokens.backgroundHighest,zIndex:-1})]}),M)}])},50696,e=>{"use strict";var t=e.i(389959),r=e.i(598525),n=e.i(934982),i=e.i(551904),a=e.i(452317);e.s(["useElementDomInfo",0,function(e){let[i,a]=(0,t.useState)(null),o=(0,n.default)(()=>{e&&a({rect:e.getBoundingClientRect(),docWidth:document.documentElement.clientWidth,docHeight:document.documentElement.clientHeight})},{type:"throttle",wait:100});return(0,t.useEffect)(()=>{e&&o(e)},[e,o]),(0,t.useEffect)(()=>{if(!e)return;let t=(0,r.throttle)(()=>{a({rect:e.getBoundingClientRect(),docWidth:document.documentElement.clientWidth,docHeight:document.documentElement.clientHeight})},100);return window.addEventListener("resize",t),window.addEventListener("scroll",t),()=>{window.removeEventListener("resize",t),window.removeEventListener("scroll",t),t.cancel()}},[e]),(0,t.useEffect)(()=>{if(!e)return;let t=e.getBoundingClientRect();a({rect:t,docWidth:document.documentElement.clientWidth,docHeight:document.documentElement.clientHeight});let r=window.setInterval(()=>{let r=e.getBoundingClientRect();(r.left!==t.left||r.top!==t.top||r.width!==t.width||r.height!==t.height)&&(t=r,a({rect:r,docWidth:document.documentElement.clientWidth,docHeight:document.documentElement.clientHeight}))},500);return()=>window.clearInterval(r)},[e]),i},"useTourStepClickInteraction",0,function({targetElement:e,activeTourStep:r,goToNextStep:n,setTourPausedReason:o}){let s=(0,t.useContext)(a.SessionContext),l=(0,t.useRef)(void 0),u=(0,t.useCallback)(()=>{let e=r?.waitToAdvanceOn==="webviewPortOpened"?s?.get(i.webviewPortOpenedObservableAtom):void 0;e?(o("async"),l.current=e.subscribe(e=>{e&&o("user")})):n()},[r?.waitToAdvanceOn,n,s,o]);(0,t.useEffect)(()=>{if(null!=e&&null!=r&&r.advanceOnTargetClick)return e.addEventListener("click",u),()=>{e.removeEventListener("click",u),l.current?.(),l.current=void 0}},[e,r,u])}])},777198,e=>{"use strict";var t=e.i(389959),r=e.i(408116),n=e.i(935984);function i(e,r,i){let{data:a,loading:o}=(0,n.useTourServiceToursSeenQuery)({variables:{tours:e},skip:i?.skip}),[s,{loading:l}]=(0,n.useTourServiceDismissTourMutation)({variables:{name:e},optimisticResponse:{__typename:"RootMutationType",markTourAsSeen2:{__typename:"TourSeen",id:e,seen:!0}},onCompleted:r}),[u,{loading:c}]=(0,n.useTourServiceDismissTourMutation)({variables:{name:e},optimisticResponse:{__typename:"RootMutationType",markTourAsSeen2:{__typename:"TourSeen",id:e,seen:!1}}}),d=!!a?.currentUser?.toursSeen[0].seen;return(0,t.useMemo)(()=>({isLoading:o,isDone:d,setAsDone:()=>s({variables:{name:e}}),unsetAsDone:()=>u({variables:{name:e}}),isMutating:l||c}),[o,d,s,u,l,c,e])}e.s(["useDismissibleElement",0,function(e){let{isLoading:t,isDone:r,setAsDone:n,unsetAsDone:a}=i(e);return{isLoading:t,isDone:r,setAsDone:n,unsetAsDone:a}},"useLazyDismissibleElement",0,function(e){let i=(0,r.useApolloClient)();return(0,t.useMemo)(()=>({getIsDone:async()=>{let{data:t,error:r,errors:a}=await i.query({query:n.TourServiceToursSeenDocument,variables:{tours:e}}),o=t?.currentUser?.toursSeen[0];if(r||a?.length||!o)throw r??a?.[0]??Error("toursSeen query returned no data");return o.seen},setAsDone:()=>i.mutate({mutation:n.TourServiceDismissTourDocument,variables:{name:e},optimisticResponse:{__typename:"RootMutationType",markTourAsSeen2:{__typename:"TourSeen",id:e,seen:!0}}})}),[i,e])},"useMemoedDismissibleElement",0,i])},970311,e=>{e.v({placeholder:"DefaultOrgIcon-module__jpmNua__placeholder"})},25561,e=>{"use strict";var t=e.i(276385),r=e.i(612343),n=e.i(61732),i=e.i(970311);e.s(["DefaultOrgIcon",0,()=>(0,t.jsx)(n.View,{align:"center",justify:"center",br:"full",clsx:i.default.placeholder,children:(0,t.jsx)(r.default,{size:12})})])},970410,e=>{"use strict";var t=e.i(351623),r=e.i(444008);let n=t.gql`
    fragment OrgGroupMetadata on OrgGroup {
  id
  color
  slug
  type
  name
}
    `;r.OrgCurrentUserFragmentDoc,r.OrgMetadataFragmentDoc,e.s(["OrgGroupMetadataFragmentDoc",0,n])},130902,e=>{"use strict";var t=e.i(351623),r=e.i(444008),n=e.i(344480);e.i(975473);let i={},a=t.gql`
    fragment OrgGroupsOrgGroup on OrgGroup {
  id
  slug
  type
  name
  color
  isMember
  memberCount
  individualMember {
    user {
      id
      displayName
      fullName
      image
    }
    email
  }
  permissions {
    editPermissions
    viewPermissions
  }
}
    `,o=t.gql`
    fragment OrgGroupsConnection on OrgGroupConnection {
  pageInfo {
    hasNextPage
    nextCursor
  }
  items {
    ...OrgGroupsOrgGroup
    isScimManaged
  }
}
    ${a}`,s=t.gql`
    fragment OrgGroupsOrg on Org {
  ...OrgMetadata
  authorizations {
    createOrgGroup {
      isAuthorized
      message
    }
  }
}
    ${r.OrgMetadataFragmentDoc}`,l=t.gql`
    query OrgGroups($orgId: String, $input: OrgGroupsInput) {
  currentUser {
    __typename
    id
    org(orgId: $orgId) {
      __typename
      ... on Org {
        id
        groups(input: $input) {
          __typename
          ...OrgGroupsConnection
          ... on UserError {
            message
          }
        }
      }
      ... on NotFoundError {
        message
      }
    }
  }
}
    ${o}`;e.s(["OrgGroupsConnectionFragmentDoc",0,o,"OrgGroupsOrgFragmentDoc",0,s,"OrgGroupsOrgGroupFragmentDoc",0,a,"useOrgGroupsQuery",0,function(e){let t={...i,...e};return n.useQuery(l,t)}])},448942,e=>{"use strict";var t=e.i(975486);let r=({slug:e})=>`${t.ORG_PATH_PREFIX}${e}`,n=`${t.ORG_PATH_PREFIX}[orgSlug]`,i=n+"/groups/[groupSlug]/[groupId]";e.s(["groupBaseRouterPath",0,i,"newOrgLink",0,{href:"/pricing",as:void 0},"orgGroupLinks",0,({orgSlug:e,groupId:t,groupSlug:n})=>{let a=(({orgSlug:e,groupId:t,groupSlug:n})=>{let i=r({slug:e});return`${i}/groups/${n}/${t}`})({orgSlug:e,groupId:t,groupSlug:n});return{members:{href:a+"/members",routerPath:i+"/members",as:void 0},settings:{href:a+"/settings",routerPath:i+"/settings",as:void 0},permissions:{href:a+"/permissions",routerPath:i+"/permissions",as:void 0}}},"orgLinks",0,({slug:e})=>{let i=r({slug:e});return{home:{href:i,routerPath:`${t.ORG_PATH_PREFIX}[orgSlug]`,as:void 0},repls:{href:i+"/repls",routerPath:n+"/repls",as:void 0},library:{href:i+"/library",routerPath:n+"/library",as:void 0},environments:{href:i+"/environments",routerPath:n+"/environments",as:void 0},members:{href:i+"/members",routerPath:n+"/members",as:void 0},connectors:{href:i+"/integrations",routerPath:n+"/integrations",as:void 0},groups:{href:i+"/groups",routerPath:n+"/groups",as:void 0},settings:{href:i+"/settings",routerPath:n+"/settings",as:void 0},usage:{href:i+"/usage",routerPath:n+"/usage",as:void 0},analytics:{href:i+"/analytics",routerPath:n+"/analytics",as:void 0},routines:{href:i+"/routines",routerPath:n+"/routines",as:void 0},security:{href:i+"/security",routerPath:n+"/security",as:void 0}}},"scimOnboardingRedirectLink",0,({orgSlug:e})=>({href:`${t.ORG_PATH_PREFIX}${e}/scim-onboarding-portal`,as:void 0})])},419635,e=>{"use strict";var t=e.i(276385),r=e.i(36454),n=e.i(389959),i=e.i(859025),a=e.i(152651),o=e.i(210853),s=e.i(661594),l=e.i(532225),u=e.i(197649),c=e.i(27923),d=e.i(158627),p=e.i(643484);let f=e.i(61732).SpecializedView.a,m=(0,n.forwardRef)(function(e,n){let{props:m,className:g,styles:h,attributes:v}=(0,d.useRuiComponentProps)("ButtonLink",e),{colorway:y,disabled:_,iconLeft:T,iconRight:P,variant:E="default",size:b="default",stretch:C,text:w,secondaryText:R,href:x,as:I,prefetch:A,replace:S,scroll:O,shallow:U,alignment:k,noNextLink:M,loading:F,dataCy:D,shrink:L,translate:j,borderRadius:z,onClick:N,onAuxClick:H,...V}=m,B=(0,o.useClickIntentHandlers)({onClick:N,onAuxClick:H}),W=(0,p.getTextVariant)(b),$=(0,p.getIconSize)(b),q=p.buttonVariantToInteractiveVariant[E],G="underlined"===E||"underlinedOnHover"===E,Y=(0,l.useCreateInteractiveRuiClasses)({variant:q,colorway:G?void 0:y,loading:F,borderRadius:z,focusRingBehavior:"onCustomFocus"}),Q=(0,p.buttonRuiClasses)({stretch:C,shrink:L,alignment:k,variant:E,size:b}),X=c.tw.merge("flex",c.tw.external((0,u.default)(Y)),c.tw.external(Q),c.tw.external(g)),K=(0,s.usePressedProps)(),Z=(0,t.jsx)(p.ButtonContent,{text:w,secondaryText:R,iconLeft:T,iconRight:P,size:b,iconSize:$,textVariant:W,alignment:k,variant:E});if(!_){let e={...v,ref:n,clsx:X,style:h?{display:"flex",...h}:{display:"flex"},role:"link",translate:j,...a.INSTRUMENTED_MARKER_PROP,...(0,i.mergeProps)(K,V,B)};if(M){if("string"!=typeof x)throw Error("Expected href to be a string");return(0,t.jsx)(f,{dataCy:D,href:x,...e,children:Z})}return(0,t.jsx)(r.default,{"data-cy":D,as:I,href:x,prefetch:A,replace:S,scroll:O,shallow:U,...e,children:Z})}return(0,t.jsx)(f,{...v,dataCy:D,ref:n,"aria-disabled":_,clsx:X,role:"link",style:h?{display:"flex",...h}:{display:"flex"},translate:j,...V,children:Z})});e.s(["ButtonLink",0,m])},83630,e=>{e.v({"fade-in":"MeasureBar-module__DtVmZW__fade-in",self:"MeasureBar-module__DtVmZW__self"})},201894,e=>{"use strict";var t=e.i(276385),r=e.i(711223);e.i(214847);var n=e.i(864300),i=e.i(89148),a=e.i(27923),o=e.i(327651),s=e.i(919073),l=e.i(244945),u=e.i(61732),c=e.i(83630);function d({current:e,total:r,color:o=i.tokens.accentPrimaryDefault,disabled:s=!1,minWidthPercent:l=0}){let c=(0,n.useIntl)(),p=Math.max(function(e=0,t=0){return 0===t||0===e?0:Math.max(0,Math.min(100,e/t*100))}(e,r),l),f={backgroundColor:s?i.tokens.outlineDefault:o,opacity:0===p?0:void 0,width:p+"%"};return(0,t.jsx)(u.View,{clsx:(0,a.tw)("relative w-full h-full top-0 left-0",a.tw.on("only")(a.tw.designSystemDeviation("rounded-(--border-radius-round)"))),style:f,role:"meter",className:"measureBarProgress","aria-label":c.formatMessage({id:"rui.measureBarOutOf",defaultMessage:"{current} out of {total}"},{current:e,total:r}),"aria-valuemin":0,"aria-valuenow":e,"aria-valuemax":r})}e.s(["MeasureBar",0,function({total:e,tooltip:n,className:i,disabled:p=!1,loading:f=!1,tooltipHidden:m=!1,size:g="medium",...h}){let v=null;return n?v=n:"current"in h?v=`${h.current||"0"}/${e}`:"data"in h&&(v=h.data?.map(({current:t})=>`${t||"0"}/${e}`).join(" | ")),(0,t.jsx)(l.Tooltip,{placement:"top",tooltip:v,maxWidth:280,isDisabled:f||m,children:(n,l)=>{let m;return f?m=null:"current"in h&&void 0!==h.current?m=(0,t.jsx)(d,{total:e,disabled:p,current:h.current,color:h.color,minWidthPercent:h.minWidthPercent}):"data"in h&&(m=h.data?.map((t,n)=>(0,r.createElement)(d,{...t,key:`measure-bar-${n}`,total:e,disabled:p,minWidthPercent:h.minWidthPercent}))),(0,r.createElement)(u.View,{...n,clsx:a.tw.merge((0,a.tw)("relative flex flex-row grow shrink overflow-clip",a.tw.designSystemDeviation("rounded-(--border-radius-round)")),a.tw.variant(g)({small:"h-150 min-h-150 max-h-150",medium:"h-200 min-h-200 max-h-200"}),a.tw.external(c.default.self),a.tw.external(f?o.loadingPulse:void 0),a.tw.external(i)),innerRef:l,onClick:void 0,key:f?"loadingMeasureBar":"finishedLoadingMeasureBar",role:"group"},(0,t.jsx)(s.ShadesSurface,{clsx:(0,a.tw)("w-full h-full relative flex flex-row grow shrink",a.tw.designSystemDeviation("rounded-(--border-radius-round)")),background:!f,elevate:"1x",style:h.backgroundColor?{backgroundColor:h.backgroundColor}:void 0,children:m},"measureBarProgress"))}})}])},66742,e=>{"use strict";var t=e.i(276385),r=e.i(152651),n=e.i(210853),i=e.i(532225),a=e.i(158627),o=e.i(744006),s=e.i(223620);e.s(["PillButton",0,function(e){let{props:l,className:u,styles:c,attributes:d}=(0,a.useRuiComponentProps)("PillButton",e),{colorway:p,onClick:f,onAuxClick:m,...g}=l,h=(0,n.useClickIntentHandlers)({onClick:f,onAuxClick:m}),v=(0,i.useCreateInteractiveRuiClasses)({variant:"filled",colorway:p??"default",borderRadius:g.compact?"md":"full",focusRingBehavior:"onCustomFocus",cursorWhenEnabled:"pointer"});return(0,t.jsx)(o.Pill,{...d,...g,colorway:p,tag:"button",cursorWhenEnabled:"pointer",...r.INSTRUMENTED_MARKER_PROP,...h,clsx:(0,s.twMerge)(v,u),style:c})}])},147622,e=>{"use strict";var t=e.i(276385),r=e.i(389959),n=e.i(27923);let[,i]=(0,n.twVar)({variable:"--popper-arrow-background"}),[a,o]=(0,n.twVar)({variable:"--popper-arrow-border-color"}),[s,l]=(0,n.twVar)({variable:"--popper-arrow-z-index"}),u=(0,r.forwardRef)(function({backgroundColor:e,borderColor:r,zIndex:u,style:c},d){let p=r??e,f={...i("transparent"),...o("transparent"),...l("1")};return void 0!==e&&Object.assign(f,i(e)),void 0!==p&&Object.assign(f,o(p)),void 0!==u&&Object.assign(f,l(String(u))),(0,t.jsx)("span",{ref:d,clsx:(0,n.tw)("block pointer-events-none",n.tw.var(s("z-(--popper-arrow-z-index)")),n.tw.onCustom("[&::after]")((0,n.tw)("block size-300 rounded-tl-md border border-solid border-b-none! border-r-none! background-popper-arrow",n.tw.designSystemDeviation("content-['']"),n.tw.var(a("border-(--popper-arrow-border-color)")))),n.tw.onCustom("[[data-popper-placement^='top']_&]")("-bottom-150"),n.tw.onCustom("[[data-popper-placement^='top']_&::after]")(n.tw.designSystemDeviation("transform-[rotate(225deg)]")),n.tw.onCustom("[[data-popper-placement^='right']_&]")("-left-150"),n.tw.onCustom("[[data-popper-placement^='right']_&::after]")(n.tw.designSystemDeviation("transform-[rotate(315deg)]")),n.tw.onCustom("[[data-popper-placement^='bottom']_&]")("-top-150"),n.tw.onCustom("[[data-popper-placement^='bottom']_&::after]")(n.tw.designSystemDeviation("transform-[rotate(45deg)]")),n.tw.onCustom("[[data-popper-placement^='left']_&]")("-right-150"),n.tw.onCustom("[[data-popper-placement^='left']_&::after]")(n.tw.designSystemDeviation("transform-[rotate(135deg)]"))),style:{...c,...f}})});e.s(["PopperArrow",0,u])},884036,e=>{"use strict";var t=e.i(276385),r=e.i(330666),n=e.i(183035),i=e.i(656077),a=e.i(927600),o=e.i(602686);e.i(214847);var s=e.i(864300),l=e.i(89148),u=e.i(27923),c=e.i(643484),d=e.i(488299),p=e.i(8047),f=e.i(61732);let[m,g]=(0,u.twVar)({variable:"--Tour--background-color"}),[h,v]=(0,u.twVar)({variable:"--Tour--border-color"});function y({total:e,current:n,colorway:i}){let a=(0,s.useIntl)();return(0,t.jsxs)(f.View,{children:[(0,t.jsx)(r.VisuallyHidden,{children:(0,t.jsx)("progress",{value:n+1,max:e,"aria-label":a.formatMessage({id:"rui.tourStep",defaultMessage:"Step"})})}),(0,t.jsx)(f.View,{"aria-hidden":!0,clsx:(0,u.tw)("flex flex-row gap-200"),children:Array(e).fill(0).map((e,r)=>(0,t.jsx)(f.View,{style:{backgroundColor:r===n?i?l.colormap[i].stronger:l.tokens.foregroundDimmest:i?l.colormap[i].dimmer:l.tokens.outlineDimmest},clsx:(0,u.tw)("w-200 h-200",u.tw.designSystemDeviation("rounded-(--border-radius-round)"))},r))})]})}e.s(["Tour",0,function({title:e,subtitle:r,icon:_,content:T,containerStyle:P,colorway:E="primary",totalSteps:b=1,currentStepIndex:C=0,onDismiss:w,onInteraction:R,onCurrentStepChange:x,cypressCloseData:I,compact:A=!0,hideCloseButton:S=!1,hideNavigation:O=!1,showSingleStepDone:U=!1,styleVariant:k="default",appearance:M="default",doneText:F,doneButtonColorway:D="primary",hideBorder:L=!1}){let j,z=(0,s.useIntl)(),N=!1===E?void 0:E,H=function(e){if("grey"!==e&&"purple"!==e&&"teal"!==e)return e}(N),V=C>=b-1,B="emphasized"===k,W=B?"primary":H;B||(j=V?(0,t.jsx)(n.default,{}):(0,t.jsx)(a.default,{}));let $="default"===M,q=A&&!B?"compact":"comfortable",G=F??z.formatMessage({id:"rui.tourDone",defaultMessage:"Done"});return(0,t.jsxs)(f.View,{clsx:(0,u.tw)("relative flex flex-col",u.tw.variant(q)({compact:"p-200 gap-200",comfortable:"p-400 gap-400"}),$&&(0,u.tw)("shadow-raised",L?"border-none border-current border-image-none":(0,u.tw)("border border-solid border-image-none",u.tw.var(h("border-(--Tour--border-color)"))),u.tw.var(m("bg-(--Tour--background-color)")),u.tw.designSystemDeviation("rounded-(--border-radius-8)"))),style:{...$?{...g(N?l.colormap[N].dimmest:l.tokens.backgroundDefault),...v(N?l.colormap[N].dimmer:l.tokens.backgroundHighest)}:{},paddingRight:e||S?void 0:l.tokens.space32,...P},children:[S?null:(0,t.jsx)(d.IconButton,{alt:z.formatMessage({id:"rui.tourClose",defaultMessage:"Close"}),colorway:N,className:(0,u.tw)("absolute",u.tw.variant(q)({compact:"top-200 right-200",comfortable:"top-400 right-400"}),u.tw.on("after")((0,u.tw)("absolute -inset-200",u.tw.designSystemDeviation("[content:'']")))),onClick:()=>{R?.("close_clicked"),w()},dataCy:I,children:(0,t.jsx)(o.default,{})}),e?(0,t.jsxs)(f.View,{clsx:(0,u.tw)("flex flex-row items-center gap-200"),children:[_?(0,t.jsx)(_,{}):null,(0,t.jsxs)(f.View,{clsx:(0,u.tw)("flex flex-row items-end gap-200"),children:[(0,t.jsx)(p.Text,{variant:"subheadDefault",multiline:!1,style:B?{fontWeight:l.tokens.fontWeightBold,color:l.tokens.foregroundDefault}:void 0,children:e}),r?(0,t.jsx)(p.Text,{variant:"text",color:"dimmest",multiline:!1,className:(0,u.tw)(u.tw.designSystemDeviation("transform-[translateY(1.5px)]")),children:r}):null]})]}):null,(0,t.jsx)(f.View,{children:"function"==typeof T?(0,t.jsx)(T,{}):(0,t.jsx)(p.Text,{color:B?"dimmest":void 0,children:T})}),(b>1?x:U)&&!O?(0,t.jsxs)(f.View,{clsx:(0,u.tw)("flex flex-row items-center justify-between"),children:[(0,t.jsx)(c.Button,{style:{visibility:C<=0?"hidden":void 0},colorway:H,text:z.formatMessage({id:"rui.tourBack",defaultMessage:"Back"}),iconLeft:B?void 0:(0,t.jsx)(i.default,{}),onClick:()=>{R?.("back_clicked"),x?.(C-1)}}),b>1?(0,t.jsx)(y,{colorway:N,total:b,current:C}):null,(0,t.jsx)(c.Button,{"data-autofocus":!0,colorway:V?D||void 0:W,text:V?G:z.formatMessage({id:"rui.tourNext",defaultMessage:"Next"}),iconLeft:j,onClick:()=>{V?(R?.("done_clicked"),w()):(R?.("next_clicked"),x?.(C+1))}})]}):null]})}])},661594,e=>{"use strict";var t=e.i(729967);e.s(["usePressedProps",0,function(e){let{pressProps:r,isPressed:n}=(0,t.usePress)(e??{});return{...r,"data-pressed":!!n||void 0,"data-rac":""}}])},407595,e=>{"use strict";e.s(["selectionOutlineRuiClasses",0,["outline","outline-surface-border-regular","-outline-offset-1","shadow-control"]])},550931,e=>{e.v({glass:"GlassSurface-module__gvx5YG__glass",translucent:"GlassSurface-module__gvx5YG__translucent"})},73490,e=>{"use strict";var t=e.i(550931);let r=t.default.glass,n=t.default.translucent;e.s(["glassSurfaceClass",0,r,"translucentSurfaceClass",0,n])},764992,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment CrosisContextCurrentUser on CurrentUser {
  id
  username
}
    `,n=t.gql`
    fragment CrosisContextRepl on Repl {
  id
  language
  authorizations {
    editFileContents {
      isAuthorized
    }
  }
}
    `;e.s(["CrosisContextCurrentUserFragmentDoc",0,r,"CrosisContextReplFragmentDoc",0,n])},691565,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var n=e.i(299020);let i={},a=t.gql`
    query ThemePreferenceCurrentUser {
  currentUser {
    id
    workspacePreferences
  }
}
    `,o=t.gql`
    mutation ThemePreferenceUpdate($input: JSON!) {
  updateWorkspacePreferences(input: $input) {
    id
    workspacePreferences
  }
}
    `;e.s(["useThemePreferenceCurrentUserQuery",0,function(e){let t={...i,...e};return r.useQuery(a,t)},"useThemePreferenceUpdateMutation",0,function(e){let t={...i,...e};return n.useMutation(o,t)}])},841114,e=>{"use strict";var t=e.i(691565),r=e.i(320216);e.s(["useThemePreference",0,function(){let{showError:e}=(0,r.default)(),{data:n}=(0,t.useThemePreferenceCurrentUserQuery)({ssr:!0}),i=n?.currentUser?.__typename==="CurrentUser"?n.currentUser:null,a=i?.workspacePreferences.theme,o=null==a,[s]=(0,t.useThemePreferenceUpdateMutation)(),l=async t=>{if(!i)return;let r="system"===t?null:t;null===r&&o||(r!==a||o)&&await s({variables:{input:{theme:r}},optimisticResponse:{__typename:"RootMutationType",updateWorkspacePreferences:{__typename:"CurrentUser",id:i.id,workspacePreferences:{...i.workspacePreferences,theme:r}}},onError:()=>e("Something went wrong setting the active theme - please try again.")})};return{isSystemTheme:o,setActiveTheme:l}}])},313287,e=>{"use strict";var t=e.i(276385),r=e.i(807988),n=e.i(923242),i=e.i(293982),a=e.i(572599),o=e.i(493166),s=e.i(668461),l=e.i(277257),u=e.i(937360),c=e.i(483620),d=e.i(650032),p=e.i(427794),f=e.i(89148),m=e.i(119417);let g={[r.FileOutputType.FILE_OUTPUT_TYPE_WORD_DOCUMENT]:{Icon:a.default,color:f.tokens.accentPrimaryDefault,label:"Document",labelId:"workspace.agentAssetsFileTypeDocument"},[r.FileOutputType.FILE_OUTPUT_TYPE_TABLE]:{Icon:c.default,color:f.tokens.accentPositiveDefault,label:"Spreadsheet",labelId:"workspace.agentAssetsFileTypeSpreadsheet"},[r.FileOutputType.FILE_OUTPUT_TYPE_TEXT]:{Icon:d.default,color:f.tokens.foregroundDefault,label:"Text",labelId:"workspace.agentAssetsFileTypeText"},[r.FileOutputType.FILE_OUTPUT_TYPE_SLIDES]:{Icon:i.default,color:f.tokens.orangeDefault,label:"Slides",labelId:"workspace.agentAssetsFileTypeSlides"},[r.FileOutputType.FILE_OUTPUT_TYPE_IMAGE]:{Icon:o.default,color:f.tokens.foregroundDefault,label:"Image",labelId:"workspace.agentAssetsFileTypeImage"},[r.FileOutputType.FILE_OUTPUT_TYPE_VIDEO]:{Icon:u.default,color:f.tokens.accentNegativeDefault,label:"Video",labelId:"workspace.agentAssetsFileTypeVideo"},[r.FileOutputType.FILE_OUTPUT_TYPE_AUDIO]:{Icon:l.default,color:f.tokens.foregroundDefault,label:"Audio",labelId:"workspace.agentAssetsFileTypeAudio"},[r.FileOutputType.FILE_OUTPUT_TYPE_HTML]:{Icon:s.default,color:f.tokens.foregroundDefault,label:"HTML",labelId:"workspace.agentAssetsFileTypeHtml"},[r.FileOutputType.FILE_OUTPUT_TYPE_CHART]:{Icon:n.default,color:f.tokens.foregroundDefault,label:"Chart",labelId:"workspace.agentChat2.feed.presentedChartLabel"},[r.FileOutputType.FILE_OUTPUT_TYPE_PDF]:{Icon:a.default,color:f.tokens.accentNegativeDefault,label:"PDF",labelId:"workspace.agentAssetsFileTypePdf"},[r.FileOutputType.FILE_OUTPUT_TYPE_UNSPECIFIED]:{Icon:a.default,color:f.tokens.foregroundDefault,label:"File",labelId:"workspace.agentAssetsFileTypeFile"}};function h(e){return g[e]||g[r.FileOutputType.FILE_OUTPUT_TYPE_UNSPECIFIED]}function v(e,t,n){return e===r.FileOutputType.FILE_OUTPUT_TYPE_HTML||e===r.FileOutputType.FILE_OUTPUT_TYPE_CHART||(e===r.FileOutputType.FILE_OUTPUT_TYPE_TEXT||e===r.FileOutputType.FILE_OUTPUT_TYPE_UNSPECIFIED)&&(n?.split(";",1)[0]?.trim().toLowerCase()==="text/html"||/\.html?$/i.test(t??""))}function y(e,{filePath:t,contentType:n}={}){let i=h(e);return e!==r.FileOutputType.FILE_OUTPUT_TYPE_CHART&&v(e,t,n)?{...i,Icon:s.default,label:"HTML",labelId:"workspace.agentAssetsFileTypeHtml"}:i}function _(e){let t=e.split(";",1)[0]?.trim().toLowerCase()??"";return"text/html"===t?r.FileOutputType.FILE_OUTPUT_TYPE_HTML:t.startsWith("image/")?r.FileOutputType.FILE_OUTPUT_TYPE_IMAGE:t.startsWith("video/")?r.FileOutputType.FILE_OUTPUT_TYPE_VIDEO:t.startsWith("audio/")?r.FileOutputType.FILE_OUTPUT_TYPE_AUDIO:"application/pdf"===t?r.FileOutputType.FILE_OUTPUT_TYPE_PDF:/spreadsheet|ms-excel|csv/.test(t)?r.FileOutputType.FILE_OUTPUT_TYPE_TABLE:/wordprocessing|msword/.test(t)?r.FileOutputType.FILE_OUTPUT_TYPE_WORD_DOCUMENT:/presentation|ms-powerpoint/.test(t)?r.FileOutputType.FILE_OUTPUT_TYPE_SLIDES:t.startsWith("text/")||"application/markdown"===t?r.FileOutputType.FILE_OUTPUT_TYPE_TEXT:r.FileOutputType.FILE_OUTPUT_TYPE_UNSPECIFIED}function T(e){if(e)try{let t=e.startsWith("file://")?e.slice(7):e;return decodeURIComponent(t)}catch{return e.startsWith("file://")?e.slice(7):e}}function P(e,t="Unknown file"){return e.split("/").pop()||t}async function E(t){let r=await e.A(245431),n=r.read(t,{type:"array",sheets:0,sheetRows:100}),i=n.Sheets[n.SheetNames[0]],a=i?.["!ref"];if(void 0===a)return[];let o=r.utils.decode_range(a);return o.e.r=Math.min(o.e.r,o.s.r+100-1),o.e.c=Math.min(o.e.c,o.s.c+200-1),r.utils.sheet_to_json(i,{header:1,defval:"",range:o}).slice(0,100).map(e=>e.slice(0,200).map(e=>null!=e?String(e):""))}async function b(t){let r=(await e.A(519741)).default;return new Promise(e=>{r.parse(t.slice(0,2097152),{preview:100,complete:t=>{e(t.data.slice(0,100).map(e=>e.slice(0,200)))}})})}e.s(["FileOutputIcon",0,function({type:e,path:r,contentType:n,size:i}){let a=y(e,{filePath:r,contentType:n});return v(e,void 0,n)?(0,t.jsx)(a.Icon,{size:i}):r?(0,t.jsx)(m.default,{path:r,size:i,fallback:(0,t.jsx)(a.Icon,{color:a.color,size:i})}):(0,t.jsx)(a.Icon,{color:a.color,size:i})},"MAX_CHART_PREVIEW_SIZE",0,8388608,"MAX_DOCX_PREVIEW_SIZE",0,0xa00000,"MAX_HTML_PREVIEW_SIZE",0,5242880,"MAX_MEDIA_PREVIEW_SIZE",0,0xa00000,"MAX_TABLE_PREVIEW_SIZE",0,2097152,"MAX_TEXT_PREVIEW_SIZE",0,512e3,"contentTypeFromFileOutputType",0,function(e){switch(e){case r.FileOutputType.FILE_OUTPUT_TYPE_TABLE:return"text/csv";case r.FileOutputType.FILE_OUTPUT_TYPE_TEXT:return"text/plain";case r.FileOutputType.FILE_OUTPUT_TYPE_PDF:return"application/pdf";case r.FileOutputType.FILE_OUTPUT_TYPE_HTML:case r.FileOutputType.FILE_OUTPUT_TYPE_CHART:return"text/html";default:return"application/octet-stream"}},"fileTypeFromContentType",0,_,"formatTextPreview",0,function(e,t=2){let r=e.trim().replace(/\s+/g," "),n=100*t;return r.slice(0,n)+(r.length>n?"...":"")},"getFileOutputConfig",0,h,"getFileOutputConfigForFile",0,y,"getFilename",0,function(e,t="Unknown file"){let r=T(e);return r?P(r,t):t},"getPathFilename",0,P,"isHtmlFile",0,v,"isSupportedPreviewMime",0,function(e){return e.startsWith("video/")||e.startsWith("image/")&&!(0,p.isSvgMimeType)(e)||e.startsWith("audio/")},"normalizeFileOutputType",0,function(e,t){switch(e){case r.FileOutputType.FILE_OUTPUT_TYPE_WORD_DOCUMENT:case r.FileOutputType.FILE_OUTPUT_TYPE_TABLE:case r.FileOutputType.FILE_OUTPUT_TYPE_TEXT:case r.FileOutputType.FILE_OUTPUT_TYPE_SLIDES:case r.FileOutputType.FILE_OUTPUT_TYPE_IMAGE:case r.FileOutputType.FILE_OUTPUT_TYPE_VIDEO:case r.FileOutputType.FILE_OUTPUT_TYPE_AUDIO:case r.FileOutputType.FILE_OUTPUT_TYPE_PDF:case r.FileOutputType.FILE_OUTPUT_TYPE_HTML:case r.FileOutputType.FILE_OUTPUT_TYPE_CHART:return e;default:return _(t)}},"parseCsvToTable",0,b,"parseExcelToTable",0,E,"supportsMediaPreview",0,function(e){return e===r.FileOutputType.FILE_OUTPUT_TYPE_IMAGE||e===r.FileOutputType.FILE_OUTPUT_TYPE_VIDEO||e===r.FileOutputType.FILE_OUTPUT_TYPE_AUDIO},"supportsTextPreview",0,function(e){return e===r.FileOutputType.FILE_OUTPUT_TYPE_TEXT||e===r.FileOutputType.FILE_OUTPUT_TYPE_TABLE},"uriToPath",0,T])},384001,e=>{"use strict";var t=e.i(846128),r=e.i(544952);let n=function({defaultScore:e=1,matchScoreMultiplier:r=1}){return function({searchQuery:n,data:i,active:a}){if("group"===i.type)return null;if(!n&&a||!n)return{score:e};let o=t.default.match(n,i.label);if(!o)return null;let{score:s,ranges:l}=o;return{score:s*r,render:{ranges:l}}}}({defaultScore:1});function i({aliases:e=[],defaultScore:t=1,matchScoreMultiplier:n=1,filterLowMatchScores:a=!0}){return function({searchQuery:i,data:o,active:s}){if("group"===o.type)return null;if(!i&&s||!i)return{score:t};let l=e.map(e=>(0,r.fzfMatch)(i,e)).find(e=>null!==e),u=l??(0,r.fzfMatch)(i,o.label);if(!u)return null;let c=Math.floor(u.score*n),d=(0,r.getMinMatchScoreForQuery)({queryLength:i.length});return a&&d&&c<d?null:{score:c,render:l?void 0:{ranges:u.ranges}}}}let a=i({defaultScore:1});e.s(["createCommand",0,function(e,{commands:t,match:r}){return{data:e,match:r,commands:t}},"createFzfMatchLabel",0,i,"fzfMatchLabel",0,a,"matchLabel",0,n])},734135,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let n={},i=t.gql`
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
    `;e.s(["useGetShouldSeeFreemiumExperienceQuery",0,function(e){let t={...n,...e};return r.useQuery(i,t)}])},786864,e=>{"use strict";var t=e.i(734135);e.s(["useShouldSeeFreemiumExperience",0,function(e,r){let{data:n,loading:i,error:a,refetch:o}=(0,t.useGetShouldSeeFreemiumExperienceQuery)({variables:{replId:e},skip:r?.skip});return{shouldSeeFreemiumExperience:n?.getRepl.__typename==="Repl"&&n.getRepl.authorizations.viewFreemiumExperience.isAuthorized,loading:i,error:a,refetch:o}}])},551904,e=>{"use strict";var t=e.i(602351),r=e.i(485792),n=e.i(826771),i=e.i(489859),a=e.i(452317);let o=(0,r.createJSONStorage)(()=>({getItem:e=>i.default.get(e,"string"),setItem:(e,t)=>i.default.set(e,t),removeItem:e=>i.default.remove(e)})),s=(0,r.atomWithStorage)("areHiddenFilesVisible",!1,o),l=(0,t.atom)(null),u=(0,t.atom)((0,n.observableValue)(!1)),c=(0,t.atom)(!1);e.s(["useAreHiddenFilesVisible",0,function(){return(0,a.useValue)(s)},"useCookieWarningBannerDismissed",0,function(){return(0,a.useValue)(c)},"useGetFileRenameInProgress",0,function(){return(0,a.useGet)(l)},"useSetAreHiddenFilesVisible",0,function(){return(0,a.useSet)(s)},"useSetCookieWarningBannerDismissed",0,function(){return(0,a.useSet)(c)},"useSetFileRenameInProgress",0,function(){return(0,a.useSet)(l)},"useWebviewPortOpenedObservable",0,function(){return(0,a.useValue)(u)},"webviewPortOpenedObservableAtom",0,u])},138716,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M4.47 11.47a.75.75 0 0 0 0 1.06l7 7a.75.75 0 1 0 1.06-1.06l-5.72-5.72H19a.75.75 0 0 0 0-1.5H6.81l5.72-5.72a.75.75 0 0 0-1.06-1.06z",clipRule:"evenodd"})})}])},752539,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M19.53 11.47a.75.75 0 0 1 0 1.06l-7 7a.75.75 0 1 1-1.06-1.06l5.72-5.72H5a.75.75 0 0 1 0-1.5h12.19l-5.72-5.72a.75.75 0 0 1 1.06-1.06z",clipRule:"evenodd"})})}])},940322,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M7 7.75a.75.75 0 0 1 0-1.5h10a.75.75 0 0 1 .75.75v10a.75.75 0 0 1-1.5 0V8.81l-8.72 8.72a.75.75 0 0 1-1.06-1.06l8.72-8.72z",clipRule:"evenodd"})})}])},923242,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{d:"M5 14.25a.75.75 0 0 1 .75.75v6a.75.75 0 0 1-1.5 0v-6a.75.75 0 0 1 .75-.75M12 8.25a.75.75 0 0 1 .75.75v12a.75.75 0 0 1-1.5 0V9a.75.75 0 0 1 .75-.75M19 2.25a.75.75 0 0 1 .75.75v18a.75.75 0 0 1-1.5 0V3a.75.75 0 0 1 .75-.75"})})}])},293982,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{fillRule:"evenodd",d:"M20 2.25A2.75 2.75 0 0 1 22.75 5v14A2.75 2.75 0 0 1 20 21.75h-8A2.75 2.75 0 0 1 9.25 19V5A2.75 2.75 0 0 1 12 2.25zm-8 1.5c-.69 0-1.25.56-1.25 1.25v14c0 .69.56 1.25 1.25 1.25h8c.69 0 1.25-.56 1.25-1.25V5c0-.69-.56-1.25-1.25-1.25z",clipRule:"evenodd"}),(0,t.jsx)("path",{d:"M6 4.25a.75.75 0 0 1 .75.75v14a.75.75 0 0 1-1.5 0V5A.75.75 0 0 1 6 4.25M2 6.25a.75.75 0 0 1 .75.75v10a.75.75 0 0 1-1.5 0V7A.75.75 0 0 1 2 6.25"})]})}])},96250,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M20 2.25A2.75 2.75 0 0 1 22.75 5v12A2.75 2.75 0 0 1 20 19.75H6.828c-.29 0-.57.101-.792.283l-.092.083-2.202 2.202a1.46 1.46 0 0 1-2.488-.924l-.004-.108V5A2.75 2.75 0 0 1 4 2.25zM4 3.75A1.25 1.25 0 0 0 2.75 5v16.19l2.134-2.134a2.75 2.75 0 0 1 1.944-.806H20A1.25 1.25 0 0 0 21.25 17V5A1.25 1.25 0 0 0 20 3.75z",clipRule:"evenodd"})})}])},429662,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 2.75a9.25 9.25 0 1 0 0 18.5 9.25 9.25 0 0 0 0-18.5M1.25 12C1.25 6.063 6.063 1.25 12 1.25S22.75 6.063 22.75 12 17.937 22.75 12 22.75 1.25 17.937 1.25 12M12 6.25a.75.75 0 0 1 .75.75v4.932l3.666 2.444a.75.75 0 1 1-.832 1.248l-4-2.667a.75.75 0 0 1-.334-.624V7a.75.75 0 0 1 .75-.75",clipRule:"evenodd"})})}])},304151,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 2.25a.75.75 0 0 1 .75.75v10.19l3.72-3.72a.75.75 0 1 1 1.06 1.06l-5 5a.75.75 0 0 1-1.06 0l-5-5a.75.75 0 1 1 1.06-1.06l3.72 3.72V3a.75.75 0 0 1 .75-.75m-9 12a.75.75 0 0 1 .75.75v4A1.25 1.25 0 0 0 5 20.25h14A1.25 1.25 0 0 0 20.25 19v-4a.75.75 0 0 1 1.5 0v4A2.75 2.75 0 0 1 19 21.75H5A2.75 2.75 0 0 1 2.25 19v-4a.75.75 0 0 1 .75-.75",clipRule:"evenodd"})})}])},493166,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{fillRule:"evenodd",d:"M9 6.25a2.75 2.75 0 1 1 0 5.5 2.75 2.75 0 0 1 0-5.5m0 1.5a1.25 1.25 0 1 0 0 2.5 1.25 1.25 0 0 0 0-2.5",clipRule:"evenodd"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M19 2.25A2.75 2.75 0 0 1 21.75 5v14A2.75 2.75 0 0 1 19 21.75H5A2.75 2.75 0 0 1 2.25 19V5A2.75 2.75 0 0 1 5 2.25zM5 3.75c-.69 0-1.25.56-1.25 1.25v14c0 .69.56 1.25 1.25 1.25h.69l8.866-8.866a2.75 2.75 0 0 1 3.888-.001l1.806 1.806V5c0-.69-.56-1.25-1.25-1.25zm11.5 8.328c-.331 0-.65.132-.884.366L7.811 20.25H19c.69 0 1.25-.56 1.25-1.25v-3.69l-2.866-2.866a1.25 1.25 0 0 0-.884-.366",clipRule:"evenodd"})]})}])},965097,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{d:"M14 21.25a.75.75 0 0 1 0 1.5h-4a.75.75 0 0 1 0-1.5zM15 17.25a.75.75 0 0 1 0 1.5H9a.75.75 0 0 1 0-1.5zM12 1.25A6.75 6.75 0 0 1 18.75 8c0 1.473-.562 2.972-1.72 4.03-.75.75-1.136 1.324-1.295 2.118a.75.75 0 0 1-1.47-.296c.241-1.206.855-2.033 1.705-2.882l.028-.028c.823-.74 1.252-1.827 1.252-2.942a5.25 5.25 0 0 0-10.5 0l.009.326c.045.777.286 1.658 1.271 2.644.742.742 1.463 1.67 1.705 2.882a.75.75 0 0 1-1.47.296c-.158-.788-.637-1.46-1.295-2.118C5.68 10.741 5.33 9.5 5.264 8.442L5.25 8A6.75 6.75 0 0 1 12 1.25"})})}])},814176,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M5.751 3.253a10.75 10.75 0 1 1 9.043 19.128 10.75 10.75 0 0 1-7.302-.623 1.25 1.25 0 0 0-.658-.056l-3.367.985-.018.005a1.75 1.75 0 0 1-2.118-1.165 1.75 1.75 0 0 1-.045-.88 1 1 0 0 1 .022-.08l1.049-3.242a1.25 1.25 0 0 0-.06-.702A10.75 10.75 0 0 1 5.75 3.253m6.984-.473a9.25 9.25 0 0 0-9.046 13.286 2.75 2.75 0 0 1 .13 1.604l-.02.07-1.046 3.229a.25.25 0 0 0 .075.213.25.25 0 0 0 .233.06l3.398-.993.065-.015c.442-.088.899-.066 1.329.064l.182.062.051.021a9.25 9.25 0 0 0 11.506-3.096A9.25 9.25 0 0 0 12.735 2.78",clipRule:"evenodd"})})}])},277257,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{d:"M19 9.25a.75.75 0 0 1 .75.75v2a7.75 7.75 0 0 1-7 7.713V22a.75.75 0 0 1-1.5 0v-2.287a7.75 7.75 0 0 1-6.99-7.328L4.25 12v-2a.75.75 0 0 1 1.5 0v2l.008.31A6.25 6.25 0 0 0 12 18.25l.31-.008a6.25 6.25 0 0 0 5.932-5.932l.008-.31v-2a.75.75 0 0 1 .75-.75"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 1.25A3.75 3.75 0 0 1 15.75 5v7a3.75 3.75 0 1 1-7.5 0V5A3.75 3.75 0 0 1 12 1.25m0 1.5A2.25 2.25 0 0 0 9.75 5v7a2.25 2.25 0 0 0 4.5 0V5A2.25 2.25 0 0 0 12 2.75",clipRule:"evenodd"})]})}])},393428,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{d:"M6.5 11.25a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5M17.5 9.25a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5M8.5 6.25a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5M13.5 5.25a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 1.25c2.827 0 5.553 1.01 7.573 2.828C21.596 5.9 22.75 8.388 22.75 11A5.75 5.75 0 0 1 17 16.75h-2.25a1 1 0 0 0-.8 1.6l.3.4.1.143a2.5 2.5 0 0 1-2.1 3.857H12a10.75 10.75 0 1 1 0-21.5m0 1.5a9.25 9.25 0 1 0 0 18.5h.25a1 1 0 0 0 .8-1.6l-.3-.4a2.5 2.5 0 0 1 2-4H17l.21-.005A4.25 4.25 0 0 0 21.25 11c0-2.161-.953-4.252-2.68-5.807C16.84 3.636 14.476 2.75 12 2.75",clipRule:"evenodd"})]})}])},40916,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{d:"M12 4.25a.75.75 0 0 1 .75.75v6.25H19a.75.75 0 0 1 0 1.5h-6.25V19a.75.75 0 0 1-1.5 0v-6.25H5a.75.75 0 0 1 0-1.5h6.25V5a.75.75 0 0 1 .75-.75"})})}])},937360,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{fillRule:"evenodd",d:"M10.65 11.264c.18.023.356.083.515.175l4.066 2.352a1.4 1.4 0 0 1 .511 1.907 1.4 1.4 0 0 1-.51.511l-4.065 2.352a1.396 1.396 0 0 1-2.094-1.21v-4.703c0-.244.064-.485.186-.697l.101-.152c.111-.145.25-.268.41-.36l.164-.08c.168-.07.35-.107.533-.107zm-.077 5.907L14.326 15l-3.753-2.172z",clipRule:"evenodd"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M19 2.25A2.75 2.75 0 0 1 21.75 5v14A2.75 2.75 0 0 1 19 21.75H5A2.75 2.75 0 0 1 2.25 19V5A2.75 2.75 0 0 1 5 2.25zM3.75 19c0 .69.56 1.25 1.25 1.25h14c.69 0 1.25-.56 1.25-1.25V9.75H3.75zM5 3.75c-.69 0-1.25.56-1.25 1.25v3.25h16.5V5c0-.69-.56-1.25-1.25-1.25z",clipRule:"evenodd"})]})}])},95136,e=>{"use strict";var t=e.i(276385),r=e.i(983420),n=e.i(532764);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)(n.default,{size:24,color:"currentColor"})})}])},210796,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{fillRule:"evenodd",d:"M5.995 15.238c.632.02 1.238.243 1.73.632l.203.177.01.009c1.088 1.075 1.079 2.81.135 3.928v-.001c-.411.488-.985.856-1.556 1.135a11 11 0 0 1-1.784.666 17 17 0 0 1-2.083.452l-.036.005-.01.001H2.6a.75.75 0 0 1-.843-.841v-.005l.002-.011q0-.014.005-.036.006-.046.02-.13a17.154 17.154 0 0 1 .434-1.953c.162-.571.38-1.202.663-1.782.28-.572.648-1.148 1.137-1.559a2.93 2.93 0 0 1 1.977-.687m-.046 1.5a1.43 1.43 0 0 0-.966.335c-.26.22-.518.585-.755 1.069a9.6 9.6 0 0 0-.57 1.536c-.09.314-.161.616-.22.883.267-.06.57-.13.883-.22a9.6 9.6 0 0 0 1.537-.57c.484-.236.849-.493 1.068-.754v-.001c.477-.563.448-1.408-.043-1.893a1.43 1.43 0 0 0-.934-.385",clipRule:"evenodd"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M22.082 1.254A.75.75 0 0 1 22.75 2c0 2.788-.787 7.654-5.887 11.312l.017.062c.089.361.196.864.268 1.423a8.4 8.4 0 0 1 .054 1.808c-.057.611-.213 1.264-.578 1.811l-.001.001c-.63.942-1.841 1.51-2.731 1.832a12 12 0 0 1-1.714.48l-.03.007-.01.002h-.003A.753.753 0 0 1 11.25 20v-4.69l-2.56-2.56H4a.75.75 0 0 1-.738-.884v-.004l.003-.01a9 9 0 0 1 .116-.522c.078-.317.2-.752.37-1.222.323-.89.89-2.101 1.832-2.732.547-.365 1.2-.522 1.812-.579a8.4 8.4 0 0 1 1.808.054c.56.072 1.062.18 1.423.268q.04.011.077.02A13.63 13.63 0 0 1 22.005 1.25zm-6.564 12.908q-1.338.766-2.768 1.348v3.533c.194-.056.41-.123.63-.204.867-.314 1.657-.748 1.997-1.256.175-.262.288-.64.332-1.117a7 7 0 0 0-.048-1.478 11 11 0 0 0-.143-.826M21.226 2.77a12.13 12.13 0 0 0-9.59 5.679l-.003.006a21.3 21.3 0 0 0-1.753 3.364l2.304 2.303a21.6 21.6 0 0 0 3.398-1.745c4.477-3.002 5.492-6.974 5.644-9.607M7.535 8.29c-.478.045-.857.158-1.119.333-.507.34-.94 1.13-1.255 1.995-.08.222-.147.437-.204.631h3.535q.589-1.428 1.364-2.764a11 11 0 0 0-.844-.147 7 7 0 0 0-1.478-.048",clipRule:"evenodd"})]})}])},652830,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M19 2.25A2.75 2.75 0 0 1 21.75 5v14A2.75 2.75 0 0 1 19 21.75H5A2.75 2.75 0 0 1 2.25 19V5A2.75 2.75 0 0 1 5 2.25zM5 3.75c-.69 0-1.25.56-1.25 1.25v14c0 .69.56 1.25 1.25 1.25h3.25V3.75zm4.75 16.5H19c.69 0 1.25-.56 1.25-1.25V5c0-.69-.56-1.25-1.25-1.25H9.75z",clipRule:"evenodd"})})}])},133522,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M2 4.8A2.8 2.8 0 0 1 4.8 2h13.9a2.8 2.8 0 0 1 2.8 2.8v13.9a2.8 2.8 0 0 1-2.8 2.8H4.8A2.8 2.8 0 0 1 2 18.7zm9.25-.3a1 1 0 0 1 1-1h6.45A1.3 1.3 0 0 1 20 4.8v13.9a1.3 1.3 0 0 1-1.3 1.3h-6.45a1 1 0 0 1-1-1z",clipRule:"evenodd"})})}])},483620,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M21.75 6A3.75 3.75 0 0 0 18 2.25H6A3.75 3.75 0 0 0 2.25 6v12A3.75 3.75 0 0 0 6 21.75h12A3.75 3.75 0 0 0 21.75 18zM18 3.75A2.25 2.25 0 0 1 20.25 6v2.25H9.75v-4.5zm-9.75 0v4.5h-4.5V6A2.25 2.25 0 0 1 6 3.75zm-4.5 6h4.5v4.5h-4.5zm0 6h4.5v4.5H6A2.25 2.25 0 0 1 3.75 18zm6 4.5v-4.5h10.5V18A2.25 2.25 0 0 1 18 20.25zm10.5-6H9.75v-4.5h10.5z",clipRule:"evenodd"})})}])},650032,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{d:"M19 3.25A1.75 1.75 0 0 1 20.75 5v2a.75.75 0 0 1-1.5 0V5a.25.25 0 0 0-.25-.25h-6.25v14.5H15a.75.75 0 0 1 0 1.5H9a.75.75 0 0 1 0-1.5h2.25V4.75H5a.25.25 0 0 0-.25.25v2a.75.75 0 0 1-1.5 0V5A1.75 1.75 0 0 1 5 3.25z"})})}])},860060,e=>{"use strict";var t=e.i(276385),r=e.i(913965);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{fill:"gray",d:"M11.94 17.029h.108c1.053.005 1.784-.532 1.806-1.327.021-.833-.742-1.413-1.827-1.397v.005h-.065v-.005c-1.085-.016-1.848.57-1.826 1.397.021.795.752 1.332 1.805 1.327"}),(0,t.jsx)("path",{fill:"gray",fillRule:"evenodd",d:"M8.879 3v1.086h2.552v1.215H8.879v1.117h2.552v1.214H8.879V8.75h2.552v1.209H8.879v1.117h2.552v.629h1.123v-1.71h2.466V8.879h-2.466V7.664h2.466V6.547h-2.466V5.332h2.466V4.215h-2.466V3H19a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM15.5 15.648c.178.982.355 1.964.485 2.95.135 1.01-.736 1.649-2.208 1.708-.33.015-.664.01-.997.004-.25-.004-.5-.009-.749-.004h-.064l-.25-.004q-.29 0-.583.005c-.305.005-.61.01-.913-.001-1.472-.06-2.343-.698-2.208-1.709.13-.985.307-1.967.485-2.949q.116-.647.23-1.295a.5.5 0 0 1 .047-.156c.355-.778 1.317-1.117 3.224-1.1 1.902-.012 2.864.322 3.223 1.1a1 1 0 0 1 .049.156q.11.645.229 1.29z",clipRule:"evenodd"})]})}])},972152,e=>{"use strict";let t="razorpay";e.s(["CHECKOUT_PAYMENT_PROVIDER_RAZORPAY",0,t,"COUNTRY_PROVIDER_MAP",0,{IN:t}])},871203,e=>{"use strict";var t,r=((t={}).Initializing="initialize",t.Connecting="connect",t.GettingInitialConfig="get init config",t.ImportingMigrationTemplate="import migration template",t.MergeDotReplit="merge dot replit",t.ExecingPostMigration="exec post migration",t.UpdatingLanguage="set language",t.Finished="finished",t.Failed="error",t);e.s(["Progress",()=>r,"shouldMigrateReplToNix",0,function(e){return"nix"!==e.language}])},975486,e=>{"use strict";e.s(["ORG_NAME_MAX_LENGTH",0,50,"ORG_NAME_MIN_LENGTH",0,2,"ORG_PATH_PREFIX",0,"/t/"])}]);

//# debugId=6d14cde8-1f7b-7008-2c81-27a233a18455
//# sourceMappingURL=03ft3j4-2kkk1.js.map