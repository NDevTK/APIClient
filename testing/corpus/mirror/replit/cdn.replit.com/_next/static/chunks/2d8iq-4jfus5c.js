;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="d34501b7-c85b-3163-4719-fc4a94850df8")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,724462,e=>{"use strict";var t=e.i(610412);e.s(["default",0,function(e){return(0,t.default)({},e)}])},154900,e=>{"use strict";var t=e.i(38523),i=e.i(601150),a=e.i(58635),r=e.i(337277),n=e.i(724462),o=e.i(610412),s=e.i(847198),l=e.i(847240);e.s(["default",0,function(e,d,u){(0,l.default)(2,arguments);var c,p,g,m,f,h,y,x=(0,t.getDefaultOptions)(),b=null!=(c=null!=(p=null==u?void 0:u.locale)?p:x.locale)?c:s.default;if(!b.formatDistance)throw RangeError("locale must contain localize.formatDistance property");var v=(0,a.default)(e,d);if(isNaN(v))throw RangeError("Invalid time value");var C=(0,o.default)((0,n.default)(u),{addSuffix:!!(null==u?void 0:u.addSuffix),comparison:v});v>0?(m=(0,r.default)(d),f=(0,r.default)(e)):(m=(0,r.default)(e),f=(0,r.default)(d));var _=String(null!=(g=null==u?void 0:u.roundingMethod)?g:"round");if("floor"===_)h=Math.floor;else if("ceil"===_)h=Math.ceil;else if("round"===_)h=Math.round;else throw RangeError("roundingMethod must be 'floor', 'ceil' or 'round'");var w=f.getTime()-m.getTime(),A=w/6e4,M=(w-((0,i.default)(f)-(0,i.default)(m)))/6e4,k=null==u?void 0:u.unit;if("second"===(y=k?String(k):A<1?"second":A<60?"minute":A<1440?"hour":M<43200?"day":M<525600?"month":"year")){var S=h(w/1e3);return b.formatDistance("xSeconds",S,C)}if("minute"===y){var B=h(A);return b.formatDistance("xMinutes",B,C)}if("hour"===y){var P=h(A/60);return b.formatDistance("xHours",P,C)}if("day"===y){var I=h(M/1440);return b.formatDistance("xDays",I,C)}if("month"===y){var j=h(M/43200);return 12===j&&"month"!==k?b.formatDistance("xYears",1,C):b.formatDistance("xMonths",j,C)}else if("year"===y){var U=h(M/525600);return b.formatDistance("xYears",U,C)}throw RangeError("unit must be 'second', 'minute', 'hour', 'day', 'month' or 'year'")}])},710967,e=>{"use strict";var t=e.i(154900),i=e.i(847240);e.s(["default",0,function(e,a){return(0,i.default)(1,arguments),(0,t.default)(e,Date.now(),a)}])},73591,e=>{"use strict";var t=e.i(710967);e.s(["formatDistanceToNowStrict",()=>t.default])},866129,e=>{"use strict";var t=e.i(351623),i=e.i(344480);e.i(975473);let a={},r=t.gql`
    query TopUpModalPaymentMethod($customerId: Int!) {
  getCustomer(customerId: $customerId) {
    ... on Customer {
      id
      billing {
        ... on CustomerBilling {
          paymentMethod {
            id
            type
            accountLast4
            isPrepaid
          }
        }
      }
    }
  }
}
    `;e.s(["useTopUpModalPaymentMethodQuery",0,function(e){let t={...a,...e};return i.useQuery(r,t)}])},772180,e=>{"use strict";var t=e.i(866129);function i(e){let t=e?.getCustomer,i=t?.__typename==="Customer"?t.billing:null;return i?.__typename==="CustomerBilling"?i.paymentMethod??null:null}e.s(["paymentMethodFromTopUpQuery",0,i,"useTopUpModalPaymentMethod",0,function({customerId:e,skip:a=!1}){let{data:r,error:n,loading:o,refetch:s}=(0,t.useTopUpModalPaymentMethodQuery)({variables:{customerId:e},skip:a,fetchPolicy:"network-only",ssr:!1});return{paymentMethod:i(r),error:n,loading:o,refetch:s}}])},599200,e=>{e.v({budgetInput:"BudgetInput-module__VvBYpa__budgetInput",closeButton:"BudgetInput-module__VvBYpa__closeButton",inputContainer:"BudgetInput-module__VvBYpa__inputContainer",inputIcon:"BudgetInput-module__VvBYpa__inputIcon"})},89807,e=>{"use strict";var t=e.i(276385),i=e.i(389959),a=e.i(330666),r=e.i(602686),n=e.i(983420),o=e.i(706323),s=e.i(403649);e.i(214847);var l=e.i(20397),d=e.i(864300),u=e.i(488299),c=e.i(528710),p=e.i(33583),g=e.i(108431),m=e.i(61732),f=e.i(599200);e.s(["BudgetInput",0,({type:e,value:h,label:y,error:x,onChange:b})=>{let v=(0,d.useIntl)(),C=(0,i.useId)(),_=(0,i.useId)(),w=v.formatMessage({id:"billing.budgetInputDollars",defaultMessage:"Dollars"});return(0,t.jsxs)(m.View,{gap:8,children:[y?(0,t.jsx)(p.Label,{color:"dimmer",htmlFor:C,children:y}):null,(0,t.jsxs)(m.View,{clsx:f.default.inputContainer,row:!0,gap:2,children:[(0,t.jsxs)(n.default,{alt:w,clsx:f.default.inputIcon,children:[(0,t.jsx)(o.default,{size:24}),(0,t.jsx)(a.VisuallyHidden,{children:(0,t.jsx)(l.FormattedMessage,{id:"billing.budgetInputDollars",defaultMessage:"Dollars"})})]}),(0,t.jsx)(c.Input,{id:C,type:"number",min:0,value:h,clsx:f.default.budgetInput,"aria-describedby":_,onChange:e=>b(e.target.value)}),h?(0,t.jsx)(u.IconButton,{alt:"hard"===e?v.formatMessage({id:"billing.budgetInputClearHardLimit",defaultMessage:"Clear monthly usage limit"}):v.formatMessage({id:"billing.budgetInputClearSoftAlert",defaultMessage:"Clear monthly usage alert"}),className:f.default.closeButton,size:20,variant:"nofill",tooltipBehavior:"hidden",onClick:()=>b(""),children:(0,t.jsx)(r.default,{})}):null]}),x?(0,t.jsx)(g.StatusBanner,{id:_,icon:(0,t.jsx)(s.default,{}),text:x,colorway:"negative"}):null]})}])},589337,e=>{e.v({closeButton:"CreditBalanceDepletedUBBModal-module__26rGpW__closeButton",modalContent:"CreditBalanceDepletedUBBModal-module__26rGpW__modalContent",responsiveCompact:"CreditBalanceDepletedUBBModal-module__26rGpW__responsiveCompact",responsiveWide:"CreditBalanceDepletedUBBModal-module__26rGpW__responsiveWide"})},635951,e=>{"use strict";var t=e.i(276385),i=e.i(196786),a=e.i(15801),r=e.i(389959),n=e.i(830675),o=e.i(592542),s=e.i(602686),l=e.i(252204),d=e.i(336187),u=e.i(476652),c=e.i(761201),p=e.i(652207),g=e.i(203238),m=e.i(772180),f=e.i(275613),h=e.i(19138),y=e.i(194691),x=e.i(827606),b=e.i(207663),v=e.i(702459),C=e.i(573916),_=e.i(151027),w=e.i(955410);e.i(214847);var A=e.i(20397),M=e.i(864300),k=e.i(753451),S=e.i(415541),B=e.i(709485);e.i(450717);var P=e.i(242917),I=e.i(488299),j=e.i(528326),U=e.i(325173),D=e.i(8047),T=e.i(61732),E=e.i(654461),R=e.i(242599),O=e.i(589337);let N=(0,i.default)(()=>e.A(928785).then(({TopUpModalContent:e})=>e),{loadableGenerated:{modules:[707473]}});function L({isSalesContractCustomer:e}){return(0,t.jsxs)(T.View,{tag:"a",row:!0,gap:4,align:"center",target:"_blank",rel:"noopener noreferrer",href:e?c.LINKS_DOCS.TEAMS_BILLING_OVERVIEW:c.LINKS_DOCS.AGENT_ASSISTANT_BILLING,color:"accent",children:[(0,t.jsx)("span",{clsx:O.default.responsiveWide,children:(0,t.jsx)(A.FormattedMessage,{id:"billing.creditsExhaustedLearnMoreAboutBilling",defaultMessage:"Learn more about billing"})}),(0,t.jsx)("span",{clsx:O.default.responsiveCompact,children:(0,t.jsx)(A.FormattedMessage,{id:"billing.creditsExhaustedLearnMoreCompact",defaultMessage:"Learn more"})}),(0,t.jsx)(l.default,{})]})}function V({autoReloadConfig:e,autoTopUpOptions:i,canUseAutoReload:a,customerId:n,oneTimeTopUpOptions:o,onAutoReloadSuccess:s,onOneTimeTopUpSuccess:l,onRequestClose:d,onTopUpClose:u}){let{show:c}=(0,P.useGlobalModal)(),p=(0,r.useRef)(1),{paymentMethod:f,error:h,loading:y,refetch:b}=(0,m.useTopUpModalPaymentMethod)({customerId:n}),v=e=>{p.current+=1,e()},C=async()=>{let e=p.current,t=await c("AutoReloadPaymentMethodModal",{customerId:n,returnUrl:new URL((0,g.getTopUpManagementHref)({initialTab:"auto_top_up",initialAutoReloadEnabled:!0}),window.location.origin).toString()});return p.current!==e?null:t};return(0,t.jsx)(j.Modal,{isOpen:!0,dataAnalyticsId:"ubb_credit_balance_depleted_modal",onRequestClose:()=>v(d),preventOutsideModalClickClose:!0,maxWidth:480,children:(0,t.jsx)(T.View,{clsx:O.default.modalContent,children:(0,t.jsx)(N,{customerId:n,paymentMethod:f,autoTopUpPaymentMethod:f,isLoadingAutoTopUpPaymentMethod:y,isLoadingPaymentMethod:y,paymentMethodError:void 0!==h,retryPaymentMethod:()=>void b(),requestPaymentMethod:C,autoReloadConfig:e,onAutoReloadEnableSuccess:s,autoTopUpOptions:i,oneTimeTopUpOptions:o,canUseAutoReload:a,onOneTimeTopUpSuccess:l,entryPoint:"credit_exhausted_modal",initialTab:a?"auto_top_up":"one-time",initialAutoReloadEnabled:a,onClose:e=>v("success"===e?u:d),getOpenId:()=>p.current,header:(0,t.jsx)(x.ReachedMonthlyCreditLimitWithTopUp,{})})})})}e.s(["CreditBalanceDepletedUBBModal",0,function({notification:e,creditDepletedCustomerId:i,isPersonalCustomer:l,onDismissCreditBalanceDepletedNotifications:c,onSuspendCreditBalanceDepletedNotifications:m,onCloseCreditBalanceDepletedNotifications:N,onAcknowledgeCreditBalanceDepletedNotifications:F,onCreditPackPurchaseSuccess:q}){let G,[W,Y]=(0,r.useState)(0),z=(0,M.useIntl)(),$=(0,a.useRouter)(),{show:H}=(0,P.useGlobalModal)(),{trackClick:K}=(0,w.useTrackClick)(),Q=e.customer?.authorizations?.useCreditDepletionPayAsYouGo?.isAuthorized??!1,X=!!e.isPendingPayAsYouGoAcknowledgement,J=e.customer?.authorizations?.recoverCreditDepletionPayAsYouGo?.isAuthorized??!1,Z=Q||X,ee=!e.isPayAsYouGoPaymentDelinquent&&(X?J:Q),et=ee&&Z,ei=Q&&!X,ea=async()=>{await c(e.id)},er=async()=>{try{if(ei)return void await m(e.id);if(Z)return void await N(e.id);await ea()}catch(e){n.captureException(e)}},en=async()=>{try{if(et)return void await F(e.id);if(X)return;await ea()}catch(e){n.captureException(e)}},eo=async()=>{try{await N(e.id)}catch(e){n.captureException(e)}},es=e.customer?.authorizations?.purchaseCreditPack?.isAuthorized??!1,el=e.customer?.authorizations?.isTopUpCustomer??!1,ed=e.customer?.authorizations?.startAutoTopUp?.isAuthorized??!1,eu=e.customer?.billing,ec=eu?.__typename==="CustomerBilling"&&"arrangement"in eu?eu.arrangement:null,ep=ec?.__typename==="CustomerSalesContract"&&"isTrial"in ec&&"canSelfServeEnterpriseBilling"in ec&&!ec.isTrial&&!1===ec.canSelfServeEnterpriseBilling,eg=e.customer?.subscriptionSummary,em=eg?.__typename==="CustomerSubscriptionSummarySelfServe"&&"plan"in eg?eg.plan:null,ef=em?.__typename==="CustomerSubscriptionSummaryTieredSelfServePlan"&&"tier"in em&&"string"==typeof em.tier&&(0,u.isActiveProTopUpTier)(em.tier)?em.tier:null,eh=ef?(0,u.getProOneTimeTopUpOptions)(ef):null,ey=ef?(0,u.getProAutoTopUpOptions)(ef):void 0,ex=eh?.defaultAmountCents??u.DEFAULT_ONE_TIME_TOP_UP_AMOUNT_CENTS,eb=e.customer?.topUpState,ev=eb?.__typename==="CustomerTopUpState"&&"topUpConfig"in eb&&null!==eb.topUpConfig,eC=ed||ev,e_=(0,k.useIsInBonsaiWebview)(),ew=(0,_.useCurrentUserStoredOrgContext)(),eA=l&&!ew.loading&&!ew.orgId,eM=!!e.isPayAsYouGoPaymentDelinquent,ek=!eM&&eA&&el,eS=!eM&&eA&&!e_&&!el&&es,eB=X&&!J&&!eM&&!ek&&!eS,eP=eM||eB,eI=["credit_pack_5","credit_pack_10","credit_pack_25"],ej=eI[0],[eU,eD]=(0,r.useState)({kind:"pack",packId:ej}),eT=eC?{kind:"autoReload"}:{kind:"topUp",amountCents:ex},{url:eE,loading:eR}=(0,C.useLiveReplAppPreviewUrl)({skip:!l||ek||(0,o.isConversationPath)($.asPath)}),eO=async e=>{K({productArea:"billing",target:"exhausted_modal_buy_credit_pack_button"}),(0,S.track)(B.events.CREDIT_PACK,{action:"exhausted_modal_pack_purchase_clicked",pack_id:e});try{Z||ea().catch(e=>{n.captureException(e)}),H("PurchaseCreditPackModal",{initialPackId:e,initialStep:"confirmation",hideReturnToPackSelection:!0,forcePersonalPurchase:!0,onPurchaseSuccess:q})}catch(e){n.captureException(e)}},eN=async e=>{if(ek&&0===W)return void(((0,S.track)(B.events.CREDIT_PACK,{action:"exhausted_modal_topup_cta_clicked",topup_kind:eT.kind,topup_amount_cents:"topUp"===eT.kind?eT.amountCents:void 0}),!Z&&ea().catch(e=>{n.captureException(e)}),e_)?(0,g.openTopUpManagementPage)({initialTab:"topUp"===eT.kind?"one-time":"auto_top_up",initialAmountCents:"topUp"===eT.kind?eT.amountCents:void 0,initialOneTimeConfirm:"topUp"===eT.kind,initialAutoReloadEnabled:"autoReload"===eT.kind}):H("TopUpModal",{customerId:i,entryPoint:"credit_exhausted_modal",autoReloadConfig:eb?.__typename==="CustomerTopUpState"&&"topUpConfig"in eb?(0,p.autoReloadConfigFromUsd)(eb.topUpConfig):null,initialTab:"topUp"===eT.kind?"one-time":"auto_top_up",initialAutoReloadEnabled:"autoReload"===eT.kind||void 0,initialAmountCents:"topUp"===eT.kind?eT.amountCents:void 0,initialOneTimeStep:"topUp"===eT.kind?"confirmation":void 0,onSuccess:X?q:void 0,onOneTimeTopUpSuccess:q}));if(eS&&0===W){if("pack"===eU.kind)return void await eO(eU.packId);(0,S.track)(B.events.CREDIT_PACK,{action:"exhausted_modal_continue_payg_clicked"})}1===W&&!l||2===W&&l?await e():Y(e=>e+1)};return(G=ep?(0,t.jsx)(b.ReachedSalesContractCreditLimit,{customerName:e.customer?.name??void 0}):ek?(0,t.jsx)(x.ReachedMonthlyCreditLimitWithTopUp,{}):eS?(0,t.jsx)(y.ReachedMonthlyCreditLimitWithCreditPacks,{appPreviewImageLoading:eR,appPreviewImageUrl:eE,billingPeriodEndDate:e.billingPeriodEnd,availablePackIds:eI,initialPackId:ej,selection:eU,onSelectionChange:eD,allowPayAsYouGo:!X||ee}):eM?(0,t.jsx)(f.PaymentDelinquentButton,{}):eB?(0,t.jsx)(E.default,{}):(0,t.jsx)(h.ReachedMonthlyCreditLimit,{appPreviewImageLoading:eR,appPreviewImageUrl:eE,customerName:e.customer?.name??void 0,isPersonalCustomer:l}),ek&&!e_)?(0,t.jsx)(V,{autoReloadConfig:eb?.__typename==="CustomerTopUpState"&&"topUpConfig"in eb?(0,p.autoReloadConfigFromUsd)(eb.topUpConfig):null,autoTopUpOptions:ey,canUseAutoReload:eC,customerId:i,oneTimeTopUpOptions:eh??void 0,onAutoReloadSuccess:q,onOneTimeTopUpSuccess:q,onRequestClose:()=>{ea().catch(e=>n.captureException(e))},onTopUpClose:()=>void eo()},i):(0,t.jsx)(j.Modal,{isOpen:!0,dataAnalyticsId:"ubb_credit_balance_depleted_modal",onRequestClose:()=>{er()},preventClose:Z,preventOutsideModalClickClose:!0,noPadding:!0,maxWidth:"600px",children:(0,t.jsxs)(T.View,{clsx:O.default.modalContent,children:[Z?(0,t.jsx)(I.IconButton,{alt:z.formatMessage({id:"rui.modalClose",defaultMessage:"Close"}),clsx:O.default.closeButton,"data-analytics-id":"ubb_payg_prompt_close_button",onClick:()=>{er()},children:(0,t.jsx)(s.default,{size:16})}):null,(0,t.jsxs)(U.default,{stepIndex:W,onNextStep:()=>{eN(async()=>{await en()})},onPrevStep:()=>Y(e=>e-1),nextButtonProps:{hidden:eP},footerLeading:(0,t.jsx)(L,{isSalesContractCustomer:ep}),hideStepIndicator:ek||eP,children:[G,(0,t.jsx)(v.SetCustomerUsageAlert,{customerId:i,initialSettings:function(e){let t=e.customer?.usageInterval?.spendingControls;if(t?.__typename==="CustomerSpendingControls"&&"alerts"in t)return t.alerts}(e),onDone:()=>{eN(async()=>{await en()})}}),l?(0,t.jsxs)(T.View,{gap:16,children:[(0,t.jsxs)(T.View,{row:!0,align:"center",gap:8,children:[(0,t.jsx)(d.default,{size:20}),(0,t.jsx)(D.Text,{variant:"subheadDefault",children:(0,t.jsx)(A.FormattedMessage,{id:"billing.creditsExhaustedReferAndEarn",defaultMessage:"Refer and earn more credits!"})})]}),(0,t.jsx)(R.default,{trackingContext:"usage-page-notification-modal"})]}):null]})]})})}])},568644,e=>{"use strict";var t=e.i(351623),i=e.i(299020);let a={},r=t.gql`
    fragment EditUsageBasedBillingAlertsFormOrg on Org {
  id
  name
}
    `,n=t.gql`
    fragment EditUsageBasedBillingAlertsFormInitialConfig on CustomerAlerts {
  hardAlert {
    id
    threshold
  }
  softAlert {
    id
    threshold
  }
}
    `,o=t.gql`
    fragment EditUsageBasedBillingAlertsFormCustomerAlerts on Customer {
  id
  usageInterval {
    spendingControls {
      ... on CustomerSpendingControls {
        alerts {
          ...EditUsageBasedBillingAlertsFormInitialConfig
        }
      }
    }
  }
}
    ${n}`,s=t.gql`
    mutation EditUsageBasedBillingAlertsFormOrgUpdateAlerts($input: UpdateCustomerSpendingAlertsInput!) {
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
    `;e.s(["EditUsageBasedBillingAlertsFormCustomerAlertsFragmentDoc",0,o,"EditUsageBasedBillingAlertsFormOrgFragmentDoc",0,r,"useEditUsageBasedBillingAlertsFormOrgUpdateAlertsMutation",0,function(e){let t={...a,...e};return i.useMutation(s,t)}])},275613,e=>{"use strict";var t=e.i(276385),i=e.i(908796),a=e.i(549645),r=e.i(334028),n=e.i(884214),o=e.i(644647),s=e.i(151027);e.i(214847);var l=e.i(864300),d=e.i(145315),u=e.i(61732);function c({billingDestination:e,openBillingTab:r,orgRole:o}){let s,p=(0,l.useIntl)(),g=(0,n.useAutoLogView)({elementId:"payment_delinquent_banner"}),m=o===i.SystemOrgGroupType.SystemMembers||o===i.SystemOrgGroupType.SystemGuests||o===i.SystemOrgGroupType.SystemViewers;return s=m?p.formatMessage({id:"billing.paymentDelinquentRazorpayNonAdmin",defaultMessage:"Your team has a failed payment. Please contact your team admins to resolve outstanding invoices."}):o===i.SystemOrgGroupType.SystemAdmins?p.formatMessage({id:"billing.paymentDelinquentRazorpayAdmin",defaultMessage:"Your team has a failed payment. Please pay your outstanding invoices to continue using AI services."}):p.formatMessage({id:"billing.paymentDelinquentRazorpayPersonal",defaultMessage:"You have a failed payment. Please pay your outstanding invoices to continue using AI services."}),(0,t.jsx)(u.View,{p:8,innerRef:g,"data-analytics-id":"payment_delinquent_banner",children:(0,t.jsx)(d.StatusBannerButton,{dataAnalyticsId:"payment_delinquent_button",dataAnalyticsDestination:m?void 0:e,iconLeft:(0,t.jsx)(a.default,{}),text:s,colorway:"negative",onClick:m?()=>{}:r})})}function p({onClick:e,orgRole:o,orgId:s,orgSlug:c,shouldTrackView:g}){let m=(0,l.useIntl)(),f=(0,n.useAutoLogView)({elementId:"payment_delinquent_banner"});switch(o){case i.SystemOrgGroupType.SystemAdmins:return(0,t.jsx)(u.View,{p:8,innerRef:g?f:void 0,"data-analytics-id":"payment_delinquent_banner",children:(0,t.jsx)(d.StatusBannerButton,{dataAnalyticsId:"payment_delinquent_button",iconLeft:(0,t.jsx)(a.default,{}),text:m.formatMessage({id:"billing.paymentDelinquentLegacyAdmin",defaultMessage:"Your team has a failed payment. Please update your team's payment method to continue using AI services."}),colorway:"negative",onClick:e??(()=>{}),...!e?{href:`/orb-customer-portal/org/${s}`,target:"_blank"}:{}})});case i.SystemOrgGroupType.SystemMembers:case i.SystemOrgGroupType.SystemGuests:case i.SystemOrgGroupType.SystemViewers:return(0,t.jsx)(u.View,{p:8,innerRef:g?f:void 0,"data-analytics-id":"payment_delinquent_banner",children:(0,t.jsx)(d.StatusBannerButton,{dataAnalyticsId:"payment_delinquent_button",iconLeft:(0,t.jsx)(r.default,{}),text:m.formatMessage({id:"billing.paymentDelinquentLegacyMember",defaultMessage:"Your team has a failed payment. Please contact your team admins to address outstanding invoices to continue using AI services."}),colorway:"negative",onClick:e??(()=>{}),...!e&&{href:`/t/${c}/members`,target:"_blank"}})});default:return(0,t.jsx)(u.View,{p:8,innerRef:g?f:void 0,"data-analytics-id":"payment_delinquent_banner",children:(0,t.jsx)(d.StatusBannerButton,{dataAnalyticsId:"payment_delinquent_button",iconLeft:(0,t.jsx)(a.default,{}),text:m.formatMessage({id:"billing.paymentDelinquentLegacyPersonal",defaultMessage:"You have a failed payment. Please update your payment method to continue using AI services."}),colorway:"negative",onClick:e??(()=>{}),...!e&&{href:"/account#billing",target:"_blank"}})})}}e.s(["PaymentDelinquentButton",0,function({onClick:e}){let{orgRole:i,orgId:a,orgSlug:r}=(0,s.useCurrentUserStoredOrgContext)(),n=(0,o.usePayInvoicesRedirect)();return n.shouldRedirect?(0,t.jsx)(c,{billingDestination:n.billingDestination,openBillingTab:n.openBillingTab,orgRole:i}):(0,t.jsx)(p,{onClick:e,orgRole:i,orgId:a,orgSlug:r,shouldTrackView:!n.isLoading})}])},18941,e=>{"use strict";var t=e.i(351623),i=e.i(568644);let a=t.gql`
    fragment TotalUsageOrg on Org {
  id
  ...EditUsageBasedBillingAlertsFormOrg
  customer {
    ... on Customer {
      ...EditUsageBasedBillingAlertsFormCustomerAlerts
    }
    ... on Error {
      message
    }
  }
}
    ${i.EditUsageBasedBillingAlertsFormOrgFragmentDoc}
${i.EditUsageBasedBillingAlertsFormCustomerAlertsFragmentDoc}`;e.s(["TotalUsageOrgFragmentDoc",0,a])},726979,e=>{e.v({bodyText:"ReachedMonthlyCreditLimit-module__RGotgG__bodyText",headerGrid:"ReachedMonthlyCreditLimit-module__RGotgG__headerGrid",headerIconWrap:"ReachedMonthlyCreditLimit-module__RGotgG__headerIconWrap",headline:"ReachedMonthlyCreditLimit-module__RGotgG__headline",heroFrame:"ReachedMonthlyCreditLimit-module__RGotgG__heroFrame",heroImage:"ReachedMonthlyCreditLimit-module__RGotgG__heroImage",heroLoadingOverlay:"ReachedMonthlyCreditLimit-module__RGotgG__heroLoadingOverlay",sublineRow:"ReachedMonthlyCreditLimit-module__RGotgG__sublineRow"})},19138,e=>{"use strict";var t=e.i(276385),i=e.i(389959),a=e.i(752539),r=e.i(269848),n=e.i(76112);e.i(214847);var o=e.i(800686),s=e.i(20397),l=e.i(864300),d=e.i(89148),u=e.i(325173),c=e.i(8047),p=e.i(61732),g=e.i(726979);let m=(0,o.defineMessages)({subline:{id:"billing.creditsDepletedContractSubline",defaultMessage:"Your account used all its included credits. Any additional usage is billed according to the terms of your plan. To review your team's usage or set a spending limit, visit your usage page."},sublineWithName:{id:"billing.creditsDepletedContractSublineWithName",defaultMessage:"Your account, {customerName}, used all its included credits. Any additional usage is billed according to the terms of your plan. To review your team's usage or set a spending limit, visit your usage page."},personalSubline:{id:"billing.creditsDepletedPersonalSubline",defaultMessage:"You used all your included credits. Any additional usage is billed according to the terms of your plan. To review your usage or set a spending limit, visit your usage page."},supportHelp:{id:"billing.creditsDepletedSupportHelp",defaultMessage:"If you'd like to add credits or have questions about how additional usage is billed, contact our support team."}});e.s(["ReachedMonthlyCreditLimit",0,function({appPreviewImageUrl:e,appPreviewImageLoading:o=!1,customerName:f,isPersonalCustomer:h}){let y=(0,l.useIntl)();(0,u.useDialogStep)({nextButtonProps:{text:y.formatMessage({id:"billing.monthlyCreditLimitKeepBuilding",defaultMessage:"Keep building"}),iconRight:(0,t.jsx)(a.default,{})}});let x=!!e,[b,v]=(0,i.useState)(!1),[C,_]=(0,i.useState)(!1);(0,i.useEffect)(()=>{v(!1),_(!1)},[e]);let w=m.subline;return h?w=m.personalSubline:f&&(w=m.sublineWithName),(0,t.jsxs)(p.View,{gap:20,children:[(0,t.jsxs)(p.View,{clsx:g.default.headerGrid,align:"start",width:"100%",children:[(0,t.jsx)(p.View,{clsx:g.default.headerIconWrap,align:"center",children:(0,t.jsx)(n.default,{"aria-hidden":!0,color:d.tokens.foregroundDefault,size:18})}),(0,t.jsx)(c.Text,{clsx:g.default.headline,variant:"subheadDefault",children:(0,t.jsx)(s.FormattedMessage,{id:"billing.monthlyCreditLimitHeadline",defaultMessage:"Look at you go! You've used your credits."})}),(0,t.jsx)(p.View,{clsx:g.default.sublineRow,children:(0,t.jsx)(c.Text,{variant:"text",color:"dimmer",children:(0,t.jsx)(s.FormattedMessage,{...w,values:{customerName:f}})})})]}),o||x&&!C?(0,t.jsxs)(p.View,{clsx:g.default.heroFrame,br:12,width:"100%",children:[x&&!C?(0,t.jsx)("img",{alt:"",clsx:g.default.heroImage,decoding:"async",src:e,style:{opacity:+!!b},onError:()=>_(!0),onLoad:()=>v(!0)}):null,o||x&&!C&&!b?(0,t.jsx)(p.View,{clsx:g.default.heroLoadingOverlay,align:"center",justify:"center",children:(0,t.jsx)(r.default,{size:24,color:d.tokens.foregroundDimmer})}):null]}):null,h?(0,t.jsx)(c.Text,{clsx:g.default.bodyText,color:"dimmer",children:(0,t.jsx)(s.FormattedMessage,{...m.supportHelp})}):null]})}])},392806,e=>{e.v({headerGrid:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__headerGrid",headline:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__headline",heroFrame:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__heroFrame",heroImage:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__heroImage",heroLoadingOverlay:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__heroLoadingOverlay",optionIconWell:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__optionIconWell",optionRow:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__optionRow",optionRowButton:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__optionRowButton",optionRowMain:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__optionRowMain",optionRowSelected:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__optionRowSelected",responsiveCompact:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__responsiveCompact",responsiveWide:"ReachedMonthlyCreditLimitWithCreditPacks-module__BNrAyq__responsiveWide"})},194691,e=>{"use strict";var t=e.i(276385),i=e.i(389959),a=e.i(752539),r=e.i(429662),n=e.i(549645),o=e.i(269848),s=e.i(76112),l=e.i(521593),d=e.i(955410);e.i(214847);var u=e.i(800686),c=e.i(614852),p=e.i(20397),g=e.i(864300),m=e.i(415541),f=e.i(709485),h=e.i(771626),y=e.i(89148),x=e.i(919073),b=e.i(39114),v=e.i(325173),C=e.i(19322),_=e.i(8047),w=e.i(61732),A=e.i(392806);let M=(0,u.defineMessages)({headline:{id:"billing.creditsExhaustedAgenticHeadline",defaultMessage:"You've used all your credits for <strong>{economyModeName} and {powerModeName} mode</strong>."},chooseOption:{id:"billing.creditsExhaustedAgenticChooseOption",defaultMessage:"Published projects are offline. Choose an option to continue building with our most powerful models."},freeMode:{id:"billing.creditsExhaustedAgenticFreeMode",defaultMessage:"You can continue to use Free mode for chats and simpler tasks."},buyCreditPackTitle:{id:"billing.creditsExhaustedBuyCreditPackTitle",defaultMessage:"Buy a credit pack"},buyCreditPackSubtitleWide:{id:"billing.creditsExhaustedBuyCreditPackSubtitleWide",defaultMessage:"One time purchase to top up Replit credits."},buyCreditPackSubtitleCompact:{id:"billing.creditsExhaustedBuyCreditPackSubtitleCompact",defaultMessage:"Top up Replit credits"},packPriceAriaLabel:{id:"billing.creditsExhaustedPackPriceAriaLabel",defaultMessage:"Credit pack price"},payAsYouGoTitleWide:{id:"billing.creditsExhaustedPayAsYouGoTitleWide",defaultMessage:"Or, continue with pay as you go"},payAsYouGoTitleCompact:{id:"billing.creditsExhaustedPayAsYouGoTitleCompact",defaultMessage:"Pay as you go"},payAsYouGoSubtitleWide:{id:"billing.creditsExhaustedPayAsYouGoSubtitleWide",defaultMessage:"Only pay for what you use. Monthly credits reset {resetDate}."},payAsYouGoSubtitleCompact:{id:"billing.creditsExhaustedPayAsYouGoSubtitleCompact",defaultMessage:"Monthly credits reset {resetDate}."},nextCtaBuyCreditPackWide:{id:"billing.creditsExhaustedNextCtaBuyCreditPackWide",defaultMessage:"Buy {price} credit pack"},nextCtaBuyCreditPackCompact:{id:"billing.creditsExhaustedNextCtaBuyCreditPackCompact",defaultMessage:"Buy {price} pack"},nextCtaKeepBuilding:{id:"billing.creditsExhaustedNextCtaKeepBuilding",defaultMessage:"Keep building"}});e.s(["ReachedMonthlyCreditLimitWithCreditPacks",0,function({billingPeriodEndDate:e,appPreviewImageUrl:u,appPreviewImageLoading:k=!1,availablePackIds:S,initialPackId:B,selection:P,onSelectionChange:I,allowPayAsYouGo:j=!0}){let U=(0,g.useIntl)(),D=(0,h.useModelProfileDisplayName)(),T=D("economy"),E=D("power"),{trackClick:R}=(0,d.useTrackClick)(),O=(0,i.useMemo)(()=>S.map(e=>(0,l.getPackById)(e)).filter(e=>null!==e),[S]),N="pack"===P.kind?P.packId:B,L=O.find(e=>e.id===N)??O[0]??null;(0,i.useEffect)(()=>{(0,m.track)(f.events.CREDIT_PACK,{action:"exhausted_modal_viewed",default_pack_id:B})},[B]);let V=(0,i.useMemo)(()=>(0,c.formatDate)(e,{preset:"medium",locale:U.locale}),[e,U.locale]),F=(0,i.useMemo)(()=>new Intl.DateTimeFormat(U.locale,{month:"short",day:"numeric"}).format(new Date(e)),[e,U.locale]),q=(0,i.useMemo)(()=>{if("pack"===P.kind&&L){let e=(0,c.formatCurrency)(L.priceUsd,{locale:U.locale,maximumFractionDigits:0});return(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)("span",{className:A.default.responsiveWide,children:U.formatMessage(M.nextCtaBuyCreditPackWide,{price:e})}),(0,t.jsx)("span",{className:A.default.responsiveCompact,children:U.formatMessage(M.nextCtaBuyCreditPackCompact,{price:e})})]})}return U.formatMessage(M.nextCtaKeepBuilding)},[U,P.kind,L]);(0,v.useDialogStep)({nextButtonProps:{text:q,iconRight:(0,t.jsx)(a.default,{})}});let G=!!u,[W,Y]=(0,i.useState)(!1),[z,$]=(0,i.useState)(!1);(0,i.useEffect)(()=>{Y(!1),$(!1)},[u]);let H=()=>{I({kind:"pack",packId:N})};(0,i.useEffect)(()=>{j||"payg"!==P.kind||I({kind:"pack",packId:N})},[j,I,N,P.kind]);let K=e=>(0,c.formatCurrency)(e,{locale:U.locale,maximumFractionDigits:0});return(0,t.jsxs)(w.View,{gap:24,children:[(0,t.jsxs)(w.View,{clsx:A.default.headerGrid,children:[(0,t.jsx)(w.View,{row:!0,align:"center",children:(0,t.jsx)(s.default,{"aria-hidden":!0,color:y.tokens.foregroundDefault,size:18})}),(0,t.jsx)(_.Text,{clsx:A.default.headline,variant:"subheadDefault",children:(0,t.jsx)(p.FormattedMessage,{...M.headline,values:{economyModeName:T,powerModeName:E,strong:e=>(0,t.jsx)("strong",{children:e})}})})]}),k||G&&!z?(0,t.jsxs)(x.ShadesSurface,{className:A.default.heroFrame,br:12,elevate:!1,border:"subtle",children:[G&&!z?(0,t.jsx)("img",{alt:"",clsx:A.default.heroImage,decoding:"async",src:u,style:{opacity:+!!W},onError:()=>$(!0),onLoad:()=>Y(!0)}):null,k||G&&!z&&!W?(0,t.jsx)(w.View,{clsx:A.default.heroLoadingOverlay,align:"center",justify:"center",children:(0,t.jsx)(o.default,{size:24,color:y.tokens.foregroundDimmer})}):null]}):null,(0,t.jsxs)(w.View,{gap:8,children:[(0,t.jsx)(_.Text,{color:"dimmer",children:U.formatMessage(M.chooseOption)}),(0,t.jsxs)(w.View,{gap:12,children:[(0,t.jsxs)(w.View,{clsx:[A.default.optionRow,"pack"===P.kind?A.default.optionRowSelected:void 0],row:!0,align:"center",gap:12,onClick:e=>{e.target===e.currentTarget&&H()},children:[(0,t.jsxs)(w.View,{tag:"button",type:"button",clsx:A.default.optionRowMain,onClick:H,row:!0,align:"center",gap:12,grow:!0,children:[(0,t.jsx)(w.View,{row:!0,align:"center",justify:"center",shrink:0,clsx:A.default.optionIconWell,children:(0,t.jsx)(n.default,{size:20,color:y.tokens.foregroundDimmer})}),(0,t.jsxs)(w.View,{gap:2,align:"start",grow:!0,children:[(0,t.jsx)(_.Text,{variant:"subheadDefault",children:U.formatMessage(M.buyCreditPackTitle)}),(0,t.jsxs)(_.Text,{color:"dimmer",children:[(0,t.jsx)("span",{className:A.default.responsiveWide,children:U.formatMessage(M.buyCreditPackSubtitleWide)}),(0,t.jsx)("span",{className:A.default.responsiveCompact,children:U.formatMessage(M.buyCreditPackSubtitleCompact)})]})]})]}),(0,t.jsx)(w.View,{shrink:0,children:(0,t.jsx)(C.Select,{compact:!0,"aria-label":U.formatMessage(M.packPriceAriaLabel),selectedKey:"pack"===P.kind?N:null,onSelectionChange:e=>{(0,m.track)(f.events.CREDIT_PACK,{action:"exhausted_modal_pack_selected",pack_id:e}),I({kind:"pack",packId:e})},onOpenChange:e=>{e&&(R({productArea:"billing",target:"exhausted_modal_pack_dropdown"}),(0,m.track)(f.events.CREDIT_PACK,{action:"exhausted_modal_pack_dropdown_opened",default_pack_id:B}))},selectValue:()=>(0,t.jsx)(_.Text,{children:K(L?.priceUsd??10)}),children:O.map(e=>{let i=K(e.priceUsd);return(0,t.jsx)(b.BaseListBoxItem,{id:e.id,textValue:i,children:(0,t.jsx)(_.Text,{children:i})},e.id)})})})]}),j?(0,t.jsxs)(w.View,{tag:"button",type:"button",clsx:[A.default.optionRow,A.default.optionRowButton,"payg"===P.kind?A.default.optionRowSelected:void 0],row:!0,align:"center",gap:12,onClick:()=>{R({productArea:"billing",target:"exhausted_modal_continue_payg_card"}),I({kind:"payg"})},children:[(0,t.jsx)(w.View,{row:!0,align:"center",justify:"center",shrink:0,clsx:A.default.optionIconWell,children:(0,t.jsx)(r.default,{size:20,color:y.tokens.foregroundDimmer})}),(0,t.jsxs)(w.View,{gap:2,grow:!0,align:"start",children:[(0,t.jsxs)(_.Text,{variant:"subheadDefault",children:[(0,t.jsx)("span",{className:A.default.responsiveWide,children:U.formatMessage(M.payAsYouGoTitleWide)}),(0,t.jsx)("span",{className:A.default.responsiveCompact,children:U.formatMessage(M.payAsYouGoTitleCompact)})]}),(0,t.jsxs)(_.Text,{color:"dimmer",children:[(0,t.jsx)("span",{className:A.default.responsiveWide,children:U.formatMessage(M.payAsYouGoSubtitleWide,{resetDate:V})}),(0,t.jsx)("span",{className:A.default.responsiveCompact,children:U.formatMessage(M.payAsYouGoSubtitleCompact,{resetDate:F})})]})]})]}):null]}),(0,t.jsx)(_.Text,{color:"dimmer",children:U.formatMessage(M.freeMode)})]})]})}])},827606,e=>{"use strict";var t=e.i(276385);e.i(214847);var i=e.i(800686),a=e.i(864300),r=e.i(771626),n=e.i(8047),o=e.i(61732);let s=(0,i.defineMessages)({title:{id:"billing.topUpModalTitle",defaultMessage:"Look at you go! You've used your credits."},description:{id:"billing.topUpModalAgenticDescription",defaultMessage:"Choose an option to keep building with {economyModeName} and {powerModeName}, and keep your published apps online."}});e.s(["ReachedMonthlyCreditLimitWithTopUp",0,function(){let e=(0,a.useIntl)(),i=(0,r.useModelProfileDisplayName)(),l=i("economy"),d=i("power");return(0,t.jsxs)(o.View,{gap:4,pr:24,children:[(0,t.jsx)(n.Text,{variant:"subheadDefault",children:e.formatMessage(s.title,{economyModeName:l,powerModeName:d})}),(0,t.jsx)(n.Text,{color:"dimmer",children:e.formatMessage(s.description,{economyModeName:l,powerModeName:d,strong:e=>(0,t.jsx)("strong",{children:e})})})]})}])},207663,e=>{"use strict";var t=e.i(276385),i=e.i(752539),a=e.i(76112);e.i(214847);var r=e.i(800686),n=e.i(20397),o=e.i(864300),s=e.i(89148),l=e.i(325173),d=e.i(8047),u=e.i(61732);let c=(0,r.defineMessages)({subline:{id:"billing.monthlyCreditLimitSalesContractSubline",defaultMessage:"Your team has used 100% of the credits included in its plan for the current billing period. Any additional usage is billed according to the terms of your plan."},sublineWithName:{id:"billing.monthlyCreditLimitSalesContractSublineWithName",defaultMessage:"Your team has used 100% of the credits included in your {customerName} plan for the current billing period. Any additional usage is billed according to the terms of your plan."}});e.s(["ReachedSalesContractCreditLimit",0,function({customerName:e}){let r=(0,o.useIntl)();return(0,l.useDialogStep)({nextButtonProps:{text:r.formatMessage({id:"billing.monthlyCreditLimitKeepBuilding",defaultMessage:"Keep building"}),iconRight:(0,t.jsx)(i.default,{})}}),(0,t.jsxs)(u.View,{gap:20,children:[(0,t.jsxs)(u.View,{row:!0,gap:12,align:"center",children:[(0,t.jsx)(a.default,{"aria-hidden":!0,color:s.tokens.foregroundDefault,size:18}),(0,t.jsx)(d.Text,{variant:"subheadDefault",children:(0,t.jsx)(n.FormattedMessage,{id:"billing.monthlyCreditLimitSalesContractHeadline",defaultMessage:"Your team has used its included credits"})})]}),(0,t.jsx)(d.Text,{color:"dimmer",children:(0,t.jsx)(n.FormattedMessage,{...e?c.sublineWithName:c.subline,values:{customerName:e}})}),(0,t.jsx)(d.Text,{color:"dimmer",children:(0,t.jsx)(n.FormattedMessage,{id:"billing.monthlyCreditLimitSalesContractAccountManager",defaultMessage:"If you'd like to add credits or have questions about how additional usage is billed, contact your account manager."})})]})}])},386652,e=>{e.v({headerText:"SetCustomerUsageAlert-module__3oC5ta__headerText"})},702459,e=>{"use strict";var t=e.i(276385),i=e.i(269848);e.i(214847);var a=e.i(20397),r=e.i(864300),n=e.i(89807),o=e.i(408699),s=e.i(643484),l=e.i(190545),d=e.i(8047),u=e.i(61732),c=e.i(386652);function p({hasPreviousUsageAlert:e,softAlertField:o,saveAlerts:g,loading:m}){let f=(0,r.useIntl)();return(0,t.jsxs)(l.Form,{gap:8,onSubmit:e=>{e.preventDefault(),g()},children:[(0,t.jsx)(u.View,{row:!0,gap:8,align:"center",children:(0,t.jsx)(d.Text,{variant:"subheadDefault",clsx:c.default.headerText,children:e?(0,t.jsx)(a.FormattedMessage,{id:"billing.usageAlertEditPrompt",defaultMessage:"Would you like to edit your usage alert?"}):(0,t.jsx)(a.FormattedMessage,{id:"billing.usageAlertSetPrompt",defaultMessage:"Would you like to set a usage alert?"})})}),(0,t.jsx)(d.Text,{children:(0,t.jsx)(a.FormattedMessage,{id:"billing.usageAlertSetDescription",defaultMessage:"You can stay on top of your spending by setting a usage alert. Choose an amount, and we'll notify you if your spending reaches it — no interruptions, just a helpful heads-up."})}),(0,t.jsx)(n.BudgetInput,{type:"soft",value:o.value,onChange:o.setValue,error:o.error?.message,label:f.formatMessage({id:"billing.usageAlertInputLabel",defaultMessage:"Usage alert"})}),(0,t.jsx)(u.View,{row:!0,children:(0,t.jsx)(s.Button,{type:"submit",text:f.formatMessage({id:"billing.usageAlertSetButton",defaultMessage:"Set usage alert"}),colorway:"primary",loading:m,iconLeft:m?(0,t.jsx)(i.default,{}):void 0,stretch:!1})})]})}e.s(["SetCustomerUsageAlert",0,function(e){let i=!!e.initialSettings?.softAlert,{softAlertField:a,saveAlerts:r,loading:n}=(0,o.useEditCustomerSpendingAlertsForm)(e);return(0,t.jsx)(p,{hasPreviousUsageAlert:i,softAlertField:a,saveAlerts:r,loading:n})}])},961998,e=>{"use strict";var t=e.i(351623),i=e.i(730029),a=e.i(568644),r=e.i(344480);e.i(975473);let n={},o=t.gql`
    fragment UsageOverviewCurrentUserPlanStatus on CurrentUser {
  id
  ...CoreSubscriptionPlanStatus
}
    ${i.CoreSubscriptionPlanStatusFragmentDoc}`,s=t.gql`
    fragment UsageOverviewCurrentUser on CurrentUser {
  id
  ...UsageOverviewCurrentUserPlanStatus
  customer {
    ...EditUsageBasedBillingAlertsFormCustomerAlerts
  }
  paymentMethod {
    ... on PaymentMethod {
      id
      type
      accountLast4
      expirationMonth
      expirationYear
    }
  }
  billingInfo {
    planInfo {
      amount
      interval
    }
  }
  usageBasedBillingBudget {
    ... on UsageBasedBillingBudget {
      id
      hasReachedBudget
    }
    ... on UnauthorizedError {
      message
    }
  }
  usageBasedBilling {
    __typename
    ... on UserUsageBasedBillingSummary {
      capabilities {
        hasOrbCustomer
      }
    }
  }
  usageInterval {
    ... on UsageInterval {
      __typename
      startDate
      endDate
      totalAmountUsd
      subtotalAmountUsd
      planDiscountUsd
      credits {
        ... on Credits {
          availableAdditionalCredits
          availableSubscriptionCredits
          totalGrantedAdditionalCredits
          totalGrantedSubscriptionCredits
        }
        ... on Error {
          message
        }
      }
    }
  }
}
    ${o}
${a.EditUsageBasedBillingAlertsFormCustomerAlertsFragmentDoc}`,l=t.gql`
    query UsageOverviewCurrentUser {
  currentUser {
    id
    username
    timeCreated
    isSubscribed
    ...UsageOverviewCurrentUser
  }
}
    ${s}`,d=t.gql`
    query UserDetailedCredits {
  currentUser {
    id
    usageInterval {
      ... on UsageInterval {
        __typename
        detailedCredits {
          ... on DetailedCredits {
            source
            totalRemainingCredits
            totalUsedCredits
            remainingCreditsByType {
              subscription
              creditPackPurchase
              autoTopUp
              referral
              gift
              additional
            }
            usedCreditsByType {
              subscription
              creditPackPurchase
              autoTopUp
              referral
              gift
              additional
            }
            creditBlocksByType {
              creditPackPurchase {
                blockId
                creditType
                currentBalance
                effectiveDate
                expiryDate
                initialBalance
              }
              autoTopUp {
                blockId
                creditType
                currentBalance
                effectiveDate
                expiryDate
                initialBalance
              }
              referral {
                blockId
                creditType
                currentBalance
                effectiveDate
                expiryDate
                initialBalance
              }
              gift {
                blockId
                creditType
                currentBalance
                effectiveDate
                expiryDate
                initialBalance
              }
            }
          }
          ... on Error {
            message
          }
        }
      }
    }
  }
}
    `;e.s(["UsageOverviewCurrentUserDocument",0,l,"useUsageOverviewCurrentUserQuery",0,function(e){let t={...n,...e};return r.useQuery(l,t)},"useUserDetailedCreditsQuery",0,function(e){let t={...n,...e};return r.useQuery(d,t)}])},408699,e=>{"use strict";var t=e.i(389959),i=e.i(961998),a=e.i(966081),r=e.i(935126),n=e.i(791729),o=e.i(371884),s=e.i(320216);function l(e,t){let i=e.trim(),a=t.trim(),r=""!==i?Number.parseFloat(i):null,o=""!==a?Number.parseFloat(a):null;return null!=r&&Number.isNaN(r)||null!=o&&Number.isNaN(o)?{ok:!1,error:{softAlertError:Number.isNaN(r)?"Please enter a number":void 0,hardAlertError:Number.isNaN(o)?"Please enter a number":void 0}}:(0,n.validateAlertThresholds)(r,o)}e.s(["useEditCustomerSpendingAlertsForm",0,function({customerId:e,initialSettings:n,onDone:d}){let u=n?.softAlert?.threshold.toString()??"",c=n?.hardAlert?.threshold.toString()??"",p=(0,o.useFormField)(u,e=>{let t=l(e,g.value);if(!t.ok&&t.error.softAlertError)return{severity:"error",message:t.error.softAlertError}}),g=(0,o.useFormField)(c,e=>{let t=l(p.value,e);if(!t.ok&&t.error.hardAlertError)return{severity:"error",message:t.error.hardAlertError}}),{showConfirm:m,showError:f}=(0,s.default)(),h=p.validate,y=g.validate;(0,t.useEffect)(()=>{h(),y()},[p.value,g.value,h,y]);let[x,b]=(0,a.useEditCustomerSpendingAlertsMutation)({onError(e){f(e.message)},onCompleted(e){"Customer"!==e.updateCustomerSpendingAlerts.__typename?f(e.updateCustomerSpendingAlerts.message):(m("Updated usage settings"),d())},refetchQueries:[i.UsageOverviewCurrentUserDocument,r.OrgUsagePeriodInformationDocument]});return{saveAlerts:()=>{if(p.validate()||g.validate())return;let t=l(p.value,g.value);t.ok&&x({variables:{input:{customerId:e,softAlertThreshold:t.value.softAlertValue,hardAlertThreshold:t.value.hardAlertValue}}})},softAlertField:p,hardAlertField:g,loading:b.loading}}])},808295,e=>{"use strict";var t=e.i(351623),i=e.i(344480);e.i(975473);let a={},r=t.gql`
    query LiveReplAppPreviewUrl($replId: String!) {
  repl(id: $replId) {
    ... on Repl {
      id
      latestAgentScreenshotUrl
      latestAgentStatus {
        statusV2
        appImageUrl
      }
      artifacts {
        artifactId
        kind
        latestScreenshotUri
      }
    }
  }
}
    `,n=t.gql`
    query RecentReplAppPreview($count: Int!) {
  recentRepls(count: $count) {
    id
    latestAgentScreenshotUrl
    latestAgentStatus {
      statusV2
      appImageUrl
    }
    artifacts {
      artifactId
      kind
      latestScreenshotUri
    }
  }
}
    `;e.s(["useLiveReplAppPreviewUrlQuery",0,function(e){let t={...a,...e};return i.useQuery(r,t)},"useRecentReplAppPreviewQuery",0,function(e){let t={...a,...e};return i.useQuery(n,t)}])},573916,e=>{"use strict";var t=e.i(15801),i=e.i(389959),a=e.i(943427),r=e.i(908796),n=e.i(808295);let o=new Set(["/replEnvironmentDesktop","/replEnvironmentMobile","/replView"]);function s(e){if(null==e||e.latestAgentStatus?.statusV2===r.AgentStatusV2.PausedWithError)return null;let t=(e.artifacts??[]).filter(e=>(0,a.isArtifactKindPreviewable)(e.kind??"web")).map(e=>e.latestScreenshotUri).find(e=>null!=e&&""!==e);if(t)return t;let i=e.latestAgentScreenshotUrl?.trim();return i||(e.latestAgentStatus?.appImageUrl?.trim()??null)}e.s(["pickLiveReplAppPreviewUrl",0,s,"useLiveReplAppPreviewUrl",0,function({skip:e=!1}){let a=function(){let e=(0,t.useRouter)();if(!o.has(e.pathname))return null;let i=e.query.replId;return null==i?null:Array.isArray(i)?i[0]??null:String(i)}(),{data:r,loading:l}=(0,n.useRecentReplAppPreviewQuery)({variables:{count:1},skip:e,fetchPolicy:"cache-first",nextFetchPolicy:"cache-first"}),{data:d,loading:u}=(0,n.useLiveReplAppPreviewUrlQuery)({variables:{replId:a??""},skip:e||null==a}),c=(0,i.useMemo)(()=>{if(e)return null;let t=r?.recentRepls?.[0],i=t?s(t):null;if(i)return i;let a=d?.repl;return a?.__typename!=="Repl"?null:s(a)},[e,r?.recentRepls,d?.repl]);return{url:c,loading:null==c&&(l||u),replId:a}}])},437676,e=>{"use strict";var t=e.i(351623),i=e.i(344480);e.i(975473);let a={},r=t.gql`
    query AgentInputRequiresPaymentMethodInfo($orgId: String, $isOrg: Boolean!) {
  currentUser {
    id
    customer @skip(if: $isOrg) {
      id
      usageInterval {
        endDate
      }
    }
    org(orgId: $orgId) {
      ... on Org {
        id
        planInfo {
          ... on OrgPlanInfo {
            planEndDate
          }
        }
      }
      ... on Error {
        message
      }
    }
  }
}
    `;e.s(["useAgentInputRequiresPaymentMethodInfoQuery",0,function(e){let t={...a,...e};return i.useQuery(r,t)}])},257548,e=>{e.v({buttonWrapper:"AgentInputRequiresPaymentMethod-module__K-zFYa__buttonWrapper",infoText:"AgentInputRequiresPaymentMethod-module__K-zFYa__infoText"})},654461,e=>{"use strict";var t=e.i(276385),i=e.i(73591),a=e.i(437676),r=e.i(683405),n=e.i(481682);e.i(214847);var o=e.i(20397),s=e.i(864300),l=e.i(753451),d=e.i(174474),u=e.i(448942),c=e.i(638141),p=e.i(643484),g=e.i(8047),m=e.i(61732),f=e.i(257548);e.s(["default",0,function({owner:e,orgId:h}){let y=(0,s.useIntl)(),x=(0,l.useDoesBonsaiWebviewSupportFeature)("stripePayment")&&e?.type!=="org",b=(0,l.useIsInBonsaiWebview)(),v=(0,c.default)(),{data:C}=(0,a.useAgentInputRequiresPaymentMethodInfoQuery)({variables:{orgId:h,isOrg:void 0!==h}}),_=null,w="",A=C?.currentUser;if(A&&(h&&A.org?.__typename==="Org"?_=A.org.planInfo?.__typename==="OrgPlanInfo"?A.org.planInfo.planEndDate:null:h||(_=A.customer?.usageInterval?.endDate)),_){let e=new Date(_);w=(0,i.formatDistanceToNowStrict)(e,{addSuffix:!1})}let M=e?.type==="org"?`${(0,u.orgLinks)({slug:e.slug}).home.href}?${(0,d.settingsQueryString)("billing")}`:"/account#billing",k=y.formatMessage({id:"components.agentPaymentAddPaymentMethod",defaultMessage:"Add payment method"});return x?k=y.formatMessage({id:"components.agentPaymentAddMoreUsage",defaultMessage:"Add more usage"}):b&&(k=y.formatMessage({id:"components.agentPaymentAddPaymentMethodReplitCom",defaultMessage:"Add payment method on replit.com"})),(0,t.jsxs)(m.View,{gap:8,align:"center",br:8,children:[(0,t.jsx)(g.Text,{clsx:f.default.infoText,color:"dimmer",children:(0,t.jsx)(o.FormattedMessage,{id:"components.agentPaymentNoUsageLeft",defaultMessage:"No monthly {agentName} usage left{hasReset, select, yes {, resets in {duration}} other {}}<br></br>Get more usage now by adding a payment method",values:{agentName:r.AGENT_NAME,hasReset:w?"yes":"other",duration:w,br:()=>(0,t.jsx)("br",{})}})}),(0,t.jsx)(m.View,{clsx:f.default.buttonWrapper,children:(0,t.jsx)(p.Button,{onClick:()=>{x?v.showPaymentFlow({type:"setup",source:"agent_input_requires_payment_method"}):window.open(M,"_blank")},disabled:b&&!x,stretch:!0,iconLeft:(0,t.jsx)(n.default,{}),text:k})})]})}])},371884,e=>{"use strict";var t=e.i(389959),i=e.i(77135),a=e.i(295798);let r={default:200,long:1e3};e.s(["useFormField",0,function(e,n,{debounceDelay:o="default"}={}){let[s,l]=(0,t.useState)(!1),[d,u]=(0,t.useState)(e),[c,p]=(0,t.useState)(null),[g,m]=(0,t.useState)(!1),f=(0,t.useRef)(d),h=(0,a.default)(n),y=(0,a.default)(e=>{if(!e){p(null),m(!0);return}p(e),"warning"===e.severity&&m(!0)}),x=(0,t.useCallback)(function(){let e=f.current,t=h.current(e);return t instanceof Promise?t.then(t=>{f.current===e&&y.current(t)}):y.current(t),t},[h,y]),b=(0,t.useRef)((0,i.debounce)(()=>x(),r[o])),v=(0,t.useCallback)(function(e,{preventTouch:t=!1,preventValidation:i=!1}={}){e!==f.current&&(f.current=e,m(!1),p(null),u(e),i||b.current(),t||l(!0))},[]),C=(0,t.useCallback)(function(e){b.current(),b.current.flush()},[]);return(0,t.useEffect)(()=>b.current.cancel,[]),{value:d,error:c?.severity&&"error"!==c.severity?null:c,warning:c?.severity==="warning"?c:null,touched:s,setValue:v,handleBlur:C,validate:x,isValid:g,setTouched:l}}])},20639,e=>{"use strict";async function t(e,{throwOnFailure:a=!1}={}){if(!window.navigator.clipboard)return void i(e,a);try{await window.navigator.clipboard.writeText(e)}catch{i(e,a)}}function i(e,t){let i=document.activeElement instanceof HTMLElement&&document.activeElement!==document.body?document.activeElement:null,a=document.createElement("textarea");a.value=e,a.style.top="0",a.style.left="0",a.style.position="fixed",(i?.parentElement??document.body).appendChild(a);try{if(a.focus(),a.select(),!document.execCommand("copy")&&t)throw Error("Copy command failed")}finally{a.remove(),i?.focus()}}e.s(["default",0,t])},935126,e=>{"use strict";var t=e.i(351623),i=e.i(18941),a=e.i(344480);e.i(975473);let r={},n=t.gql`
    fragment OrgUsageBillingAlertsConfig on UsageBasedBillingAlertsConfig {
  hardAlert {
    id
    threshold
  }
  softAlert {
    id
    threshold
  }
  globalAlert {
    id
    threshold
  }
  groupAlerts {
    id
    groupId
    threshold
    group {
      id
      name
    }
  }
}
    `,o=t.gql`
    fragment OrgIndividualUserAlertsConfig on UsageBasedBillingAlertsConfig {
  individualUserAlerts {
    id
    groupId
    threshold
    group {
      id
      name
      type
      individualMember {
        user {
          id
        }
      }
    }
  }
}
    `,s=t.gql`
    fragment OrgUsagePeriodInformation on UsageInterval {
  startDate
  endDate
  totalAmountUsd
  subtotalAmountUsd
  credits {
    ... on Credits {
      availableAdditionalCredits
      availableSubscriptionCredits
      totalGrantedAdditionalCredits
      totalGrantedSubscriptionCredits
    }
    ... on Error {
      message
    }
  }
}
    `,l=t.gql`
    fragment OrgUsageAuthorizations on OrgAuthorizations {
  viewSubscription {
    isAuthorized
    message
  }
  viewUsage {
    isAuthorized
    message
  }
  viewUsageAlerts {
    isAuthorized
    message
  }
  editUsageAlerts {
    isAuthorized
    message
  }
  editUsageLimit {
    isAuthorized
    message
  }
}
    `,d=t.gql`
    fragment OrgUsageBasedBillingBudget on UsageBasedBillingBudget {
  id
  hasReachedBudget
}
    `,u=t.gql`
    query OrgUsagePeriodInformation($orgId: String!) {
  currentUser {
    id
    org(orgId: $orgId) {
      __typename
      ... on Org {
        id
        ...TotalUsageOrg
        usageInterval {
          ... on UsageInterval {
            ...OrgUsagePeriodInformation
          }
          ... on Error {
            message
          }
        }
        paymentMethod {
          ... on PaymentMethod {
            __typename
            id
          }
          ... on Error {
            message
          }
        }
        usageBasedBillingBudget {
          ... on UsageBasedBillingBudget {
            ...OrgUsageBasedBillingBudget
          }
          ... on Error {
            message
          }
        }
        usageBasedBillingAlerts {
          ... on UsageBasedBillingAlertsConfig {
            ...OrgUsageBillingAlertsConfig
            ...OrgIndividualUserAlertsConfig
          }
          ... on Error {
            message
          }
        }
        planInfo {
          __typename
          ... on OrgPlanInfo {
            name
            planId
            planEndDate
            planStartDate
          }
          ... on Error {
            message
          }
        }
        authorizations {
          ...OrgUsageAuthorizations
        }
      }
    }
  }
}
    ${i.TotalUsageOrgFragmentDoc}
${s}
${d}
${n}
${o}
${l}`,c=t.gql`
    query OrgDetailedCredits($orgId: String!) {
  currentUser {
    id
    org(orgId: $orgId) {
      __typename
      ... on Org {
        id
        usageInterval {
          ... on UsageInterval {
            __typename
            detailedCredits {
              ... on DetailedCredits {
                source
                totalRemainingCredits
                totalUsedCredits
                remainingCreditsByType {
                  subscription
                  creditPackPurchase
                  autoTopUp
                  referral
                  gift
                  additional
                }
                usedCreditsByType {
                  subscription
                  creditPackPurchase
                  autoTopUp
                  referral
                  gift
                  additional
                }
                creditBlocksByType {
                  creditPackPurchase {
                    blockId
                    creditType
                    currentBalance
                    effectiveDate
                    expiryDate
                    initialBalance
                  }
                  autoTopUp {
                    blockId
                    creditType
                    currentBalance
                    effectiveDate
                    expiryDate
                    initialBalance
                  }
                  referral {
                    blockId
                    creditType
                    currentBalance
                    effectiveDate
                    expiryDate
                    initialBalance
                  }
                  gift {
                    blockId
                    creditType
                    currentBalance
                    effectiveDate
                    expiryDate
                    initialBalance
                  }
                }
              }
              ... on Error {
                message
              }
            }
          }
        }
      }
    }
  }
}
    `;e.s(["OrgIndividualUserAlertsConfigFragmentDoc",0,o,"OrgUsageBillingAlertsConfigFragmentDoc",0,n,"OrgUsagePeriodInformationDocument",0,u,"useOrgDetailedCreditsQuery",0,function(e){let t={...r,...e};return a.useQuery(c,t)},"useOrgUsagePeriodInformationQuery",0,function(e){let t={...r,...e};return a.useQuery(u,t)}])},580519,e=>{"use strict";var t=e.i(276385),i=e.i(389959),a=e.i(859025),r=e.i(183035),n=e.i(402099);e.i(214847);var o=e.i(864300),s=e.i(20639),l=e.i(89148),d=e.i(643484),u=e.i(488299),c=e.i(244945);let p=({value:e,onCopy:t,tooltipText:a,tooltipSuccessText:r,successTimeout:n=1e3})=>{let l=(0,o.useIntl)(),d=a??l.formatMessage({id:"rui.copyButtonCopyTooltip",defaultMessage:"Copy"}),u=r??l.formatMessage({id:"rui.copyButtonCopiedTooltip",defaultMessage:"Copied!"}),[c,p]=(0,i.useState)(!1),g=(0,i.useRef)(null),[m,f]=(0,i.useState)(void 0);return(0,i.useEffect)(()=>{let e=g.current;return()=>{e&&clearTimeout(e)}},[]),{isCopied:c,isTooltipOpen:m,onIsTooltipOpenChange:e=>{f(c?void 0:e)},tooltip:c?u:d,copy:(0,i.useCallback)(()=>{let i="function"==typeof e?e():e;i&&((0,s.default)(i),p(!0),f(!0),g.current&&clearTimeout(g.current),g.current=setTimeout(()=>{p(!1),f(!1)},n),t?.())},[t,n,e])}};e.s(["CopyButton",0,function({textToCopy:e,successTimeout:i,onCopy:o,tooltipDisable:s,tooltipText:u,tooltipPlacement:g="bottom-end",tooltipSuccessText:m,tooltipHideWhenOutOfBounds:f,showIcon:h=!1,successIcon:y=(0,t.jsx)(r.default,{color:l.tokens.accentPositiveStronger}),copyIcon:x=(0,t.jsx)(n.default,{}),onClick:b,...v}){let{isTooltipOpen:C,onIsTooltipOpenChange:_,tooltip:w,isCopied:A,copy:M}=p({value:e,successTimeout:i,onCopy:o,tooltipDisable:s,tooltipText:u,tooltipSuccessText:m});return(0,t.jsx)(c.Tooltip,{isOpen:C,onOpenChange:_,tooltip:w,placement:g,hideWhenOutOfBounds:f,isDisabled:s,children:(0,t.jsx)(d.Button,{disabled:!e,iconLeft:h&&A?y:x,...(0,a.mergeProps)(v),onClick:e=>{M(),b?.(e)}})})},"CopyIconButton",0,function({textToCopy:e,successTimeout:i,children:a=(0,t.jsx)(n.default,{}),successIcon:o=(0,t.jsx)(r.default,{color:l.tokens.accentPositiveStronger}),tooltipDisable:s,tooltipText:d,tooltipSuccessText:c,onCopy:g,tooltipPlacement:m="bottom",...f}){let{isTooltipOpen:h,onIsTooltipOpenChange:y,isCopied:x,tooltip:b,copy:v}=p({value:e,successTimeout:i,onCopy:g,tooltipDisable:s,tooltipText:d,tooltipSuccessText:c});return(0,t.jsx)(u.IconButton,{disabled:!e,...f,isTooltipOpen:h,onTooltipOpenChange:y,alt:b,onClick:v,tooltipPlacement:m,children:x?o:a})}])},325173,e=>{"use strict";var t=e.i(276385),i=e.i(389959),a=e.i(138716),r=e.i(752539),n=e.i(269848);e.i(214847);var o=e.i(864300),s=e.i(89148),l=e.i(27923),d=e.i(643484),u=e.i(244945),c=e.i(61732);let p=(0,i.createContext)(null);function g({onNextStep:e,nextButtonProps:i,onPrevStep:n,prevButtonProps:s,stepProgress:p,footerLeading:m,hideStepIndicator:h,footerLayout:y="default",stepIndicatorEmphasis:x="default"}){let b=(0,o.useIntl)(),v=b.formatMessage({id:"rui.multiStepDialogBack",defaultMessage:"Back"}),C=b.formatMessage({id:"rui.multiStepDialogContinue",defaultMessage:"Continue"});if(null!=m){let o=p.current>0&&!0!==s.hidden,g=p.current<p.total&&!0!==i.hidden;return(0,t.jsxs)(c.View,{clsx:(0,l.tw)("flex flex-row items-center gap-200 pt-600"),children:[(0,t.jsx)(c.View,{clsx:(0,l.tw)("flex flex-row grow shrink basis-0 items-center justify-start"),children:o?(0,t.jsx)(u.Tooltip,{isDisabled:!s.disabled||!s.disabledReason,tooltip:s.disabledReason,children:(0,t.jsx)(d.Button,{iconLeft:(0,t.jsx)(a.default,{}),text:v,...s,onClick:e=>{s.onClick?.(e),n()}},"prev-step-button")}):m}),h?null:(0,t.jsx)(f,{currentStep:p.current,totalSteps:p.total,emphasis:x}),(0,t.jsx)(c.View,{clsx:(0,l.tw)("flex flex-row grow shrink basis-0 items-center justify-end"),children:g?(0,t.jsx)(u.Tooltip,{isDisabled:!i.disabled||!i.disabledReason,tooltip:i.disabledReason,children:(0,t.jsx)(d.Button,{iconRight:(0,t.jsx)(r.default,{}),colorway:"primary",type:"submit",text:C,...i,onClick:t=>{i.onClick?.(t),e()}},"next-step-button")}):(0,t.jsx)(c.View,{clsx:(0,l.tw)(l.tw.designSystemDeviation("w-[98px]"))})})]})}let _="progressLeading"===y;return(0,t.jsxs)(c.View,{clsx:(0,l.tw)("flex flex-row pt-400",_?"items-center justify-end gap-200":"justify-between"),children:[0===p.current||s.hidden?(0,t.jsx)(c.View,{clsx:_?(0,l.tw)("hidden"):(0,l.tw)(l.tw.designSystemDeviation("w-[73px]"))}):(0,t.jsx)(u.Tooltip,{isDisabled:!s.disabled||!s.disabledReason,tooltip:s.disabledReason,children:(0,t.jsx)(d.Button,{iconLeft:(0,t.jsx)(a.default,{}),text:v,...s,onClick:e=>{s.onClick?.(e),n()}},"prev-step-button")}),h?null:(0,t.jsx)(f,{currentStep:p.current,totalSteps:p.total,leading:_,emphasis:x}),p.current===p.total||i.hidden?(0,t.jsx)(c.View,{clsx:_?(0,l.tw)("hidden"):(0,l.tw)(l.tw.designSystemDeviation("w-[98px]"))}):(0,t.jsx)(u.Tooltip,{isDisabled:!i.disabled||!i.disabledReason,tooltip:i.disabledReason,children:(0,t.jsx)(d.Button,{iconRight:(0,t.jsx)(r.default,{}),colorway:"primary",type:"submit",text:C,...i,onClick:t=>{i.onClick?.(t),e()}},"next-step-button")})]})}let m={default:{backgroundColor:s.tokens.accentPrimaryDefault,filter:`drop-shadow(0px 0px 6px ${s.tokens.accentPrimaryDefault})`},subtle:{backgroundColor:s.tokens.accentPrimaryDefault}};function f({currentStep:e,totalSteps:i,leading:a=!1,emphasis:r="default"}){let n=(0,o.useIntl)();return(0,t.jsx)(c.View,{role:"progressbar","aria-label":n.formatMessage({id:"rui.multiStepDialogStepProgress",defaultMessage:"Step {current} of {total}"},{current:e+1,total:i}),"aria-valuemin":1,"aria-valuemax":i,"aria-valuenow":e+1,clsx:(0,l.tw)("flex flex-row items-center justify-center gap-100",a&&"order-first mr-auto"),children:Array.from({length:i},(i,a)=>(0,t.jsx)(c.View,{clsx:(0,l.tw)("w-150 h-150 rounded-full",l.tw.designSystemDeviation("bg-(--background-higher)")),style:a===e?m[r]:void 0},a))})}e.s(["default",0,function(e){let{children:a,onNextStep:r,onPrevStep:o,stepIndex:s,loading:d,stepProgress:u,nextButtonProps:m,prevButtonProps:f,contentContainerClassName:h,footerLeading:y,hideStepIndicator:x=!1,footerLayout:b="default",stepIndicatorEmphasis:v="default"}=e,C=i.Children.toArray(a),_=u??{current:s,total:C.length},[w,A]=(0,i.useState)({}),[M,k]=(0,i.useState)({}),S={...m,...w},B={...f,...M};function P(){A({}),k({})}let I=(0,i.useMemo)(()=>({setNextButtonProps:A,setPrevButtonProps:k}),[]);if(s<0||s>=C.length)throw Error("Invalid step index");return(0,t.jsx)(p.Provider,{value:I,children:(0,t.jsx)(c.View,{clsx:(0,l.tw)("flex grow shrink items-center justify-start overflow-auto"),children:d?(0,t.jsx)(c.View,{clsx:(0,l.tw)("flex items-center justify-center m-auto",l.tw.designSystemDeviation("min-h-[300px]")),children:(0,t.jsx)(n.default,{})}):(0,t.jsxs)(c.View,{clsx:l.tw.merge("m-auto w-full",null!=y?"p-600":"p-400",l.tw.external(h)),children:[C[s],(0,t.jsx)(g,{onNextStep:function(){P(),null==S.onClick&&r()},onPrevStep:function(){P(),null==B.onClick&&o()},nextButtonProps:S,prevButtonProps:B,stepProgress:_,footerLeading:y,hideStepIndicator:x,footerLayout:b,stepIndicatorEmphasis:v})]})})})},"useDialogStep",0,function({nextButtonProps:e,prevButtonProps:t}){let a=(0,i.useContext)(p);if(null==a)throw Error("useDialogStep must be used within a MultiStepDialog");let{setNextButtonProps:r,setPrevButtonProps:n}=a;(0,i.useEffect)(()=>{null!=e&&r(e)},[r,e]),(0,i.useEffect)(()=>{null!=t&&n(t)},[n,t])}])},145315,e=>{"use strict";var t=e.i(276385),i=e.i(36454),a=e.i(389959),r=e.i(602686),n=e.i(983420),o=e.i(152651),s=e.i(210853);e.i(214847);var l=e.i(864300),d=e.i(89148),u=e.i(27923),c=e.i(919073),p=e.i(158627),g=e.i(488299),m=e.i(8047),f=e.i(61732);let[,h]=(0,u.twVar)({variable:"--status-banner-button-background"}),[,y]=(0,u.twVar)({variable:"--status-banner-button-active-background"}),[,x]=(0,u.twVar)({variable:"--status-banner-button-color"}),[,b]=(0,u.twVar)({variable:"--status-banner-button-disabled-color"}),[,v]=(0,u.twVar)({variable:"--status-banner-button-border"}),[,C]=(0,u.twVar)({variable:"--status-banner-button-hover-border"}),[,_]=(0,u.twVar)({variable:"--status-banner-button-active-border"}),w=f.SpecializedView.button,A=(0,a.forwardRef)((e,a)=>{let{props:A,className:M,attributes:k}=(0,p.useRuiComponentProps)("StatusBannerButton",e),S=(0,l.useIntl)(),B=(0,s.useClickIntentHandlers)(),{colorway:P,closable:I,iconLeft:j,iconRight:U,text:D,closeAction:T}=A,E=(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(n.IconProvider,{size:16,children:j}),(0,t.jsx)(f.View,{clsx:(0,u.tw)("grow shrink"),children:(0,t.jsx)(m.Text,{clsx:(0,u.tw)("flex-auto text-center text-inherit"),variant:"small",children:D})}),(0,t.jsx)(n.IconProvider,{size:16,children:U})]}),R=P?d.colormap[P]:null,O={...h(R?.dimmest??d.tokens.interactiveBackground),...y(R?.dimmer??d.tokens.interactiveBackgroundActive),...x(R?.strongest??d.tokens.foregroundDefault),...b(R?.default??d.tokens.foregroundDimmest),...v(R?.dimmer??d.tokens.outlineDimmest),...C(R?.strongest??d.tokens.outlineDimmer),..._(R?.default??d.tokens.outlineDefault)};return(0,t.jsxs)(f.View,{clsx:(0,u.tw)("min-h-800 flex-row items-center gap-100 rounded-md border border-solid shadow-none duration-snappy ease-snappy",u.tw.designSystemDeviation("transition-[border-color,box-shadow] border-(--status-banner-button-border) bg-(--status-banner-button-background) text-(--status-banner-button-color)"),u.tw.on("disabled")(u.tw.designSystemDeviation("bg-(--status-banner-button-background) text-(--status-banner-button-disabled-color)")),u.tw.onCustom("[&:not([disabled]):hover]")((0,u.tw)("transition-none",u.tw.designSystemDeviation("border-(--status-banner-button-hover-border)"))),u.tw.onCustom("[&:not([disabled]):active]")((0,u.tw)("transition-none",u.tw.designSystemDeviation("border-(--status-banner-button-active-border) bg-(--status-banner-button-active-background)"))),u.tw.onCustom("[&:has(:focus-visible)]")((0,u.tw)("outline-2 outline-solid outline-offset-2 transition-none",u.tw.designSystemDeviation("border-(--status-banner-button-hover-border) outline-(--status-banner-button-color)")))),style:O,children:["href"in A?(0,t.jsx)(i.default,{...k,"data-analytics-id":A.dataAnalyticsId,"data-analytics-destination":A.dataAnalyticsDestination,as:A.as,href:A.href,prefetch:A.prefetch,replace:A.replace,scroll:A.scroll,shallow:A.shallow,ref:a,rel:A.rel,role:"link",target:A.target,...o.INSTRUMENTED_MARKER_PROP,...B,className:u.tw.merge("flex flex-auto cursor-pointer flex-row items-center justify-center gap-400 bg-transparent py-100 pl-200 text-inherit",!0===I?"pr-0":"pr-200",u.tw.external(M)),children:E}):(0,t.jsx)(w,{...k,"data-analytics-id":A.dataAnalyticsId,"data-analytics-destination":A.dataAnalyticsDestination,ref:a,onClick:e=>{B.onClick?.(e),A.onClick(e)},type:A.type,...o.INSTRUMENTED_MARKER_PROP,className:u.tw.merge("flex flex-auto cursor-pointer flex-row items-center justify-center gap-400 bg-transparent py-100 pl-200 text-inherit",!0===I?"pr-0":"pr-200",u.tw.external(M)),children:E}),I?(0,t.jsx)(c.ShadesSurface,{background:!1,clsx:(0,u.tw)("pr-100"),children:(0,t.jsx)(g.IconButton,{alt:S.formatMessage({id:"rui.statusBannerButtonClose",defaultMessage:"Close"}),tooltipBehavior:"hidden",colorway:P,onClick:T,children:(0,t.jsx)(r.default,{})})}):null]})});A.displayName="StatusBannerButton",e.s(["StatusBannerButton",0,A])},771626,e=>{"use strict";var t=e.i(389959);e.i(214847);var i=e.i(800686),a=e.i(864300);let r=(0,i.defineMessages)({free:{id:"workspace.modelProfileDisplayNameFree",defaultMessage:"Free"},lite:{id:"workspace.modelProfileDisplayNameLite",defaultMessage:"Lite"},economy:{id:"workspace.modelProfileDisplayNameEconomyAgentic",defaultMessage:"Power"},power:{id:"workspace.modelProfileDisplayNamePowerAgentic",defaultMessage:"Max"},turbo:{id:"workspace.modelProfileDisplayNameTurbo",defaultMessage:"Turbo"},auto:{id:"workspace.modelProfileDisplayNameAuto",defaultMessage:"Auto"}});function n(e){return r[e]}e.s(["getModelProfileDisplayLabel",0,n,"useModelProfileDisplayName",0,function(){let e=(0,a.useIntl)();return(0,t.useCallback)(t=>e.formatMessage(n(t)),[e])}])},481682,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(i.default,{...e,children:[(0,t.jsx)("path",{d:"M13 4.25a.75.75 0 0 1 0 1.5H4c-.69 0-1.25.56-1.25 1.25v2.25H16a.75.75 0 0 1 0 1.5H2.75V17c0 .69.56 1.25 1.25 1.25h16c.69 0 1.25-.56 1.25-1.25v-6a.75.75 0 0 1 1.5 0v6A2.75 2.75 0 0 1 20 19.75H4A2.75 2.75 0 0 1 1.25 17V7A2.75 2.75 0 0 1 4 4.25z"}),(0,t.jsx)("path",{d:"M19 1.25a.75.75 0 0 1 .75.75v2.25H22a.75.75 0 0 1 0 1.5h-2.25V8a.75.75 0 0 1-1.5 0V5.75H16a.75.75 0 0 1 0-1.5h2.25V2a.75.75 0 0 1 .75-.75"})]})}])},402099,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(i.default,{...e,children:[(0,t.jsx)("path",{fillRule:"evenodd",d:"M20 7.25A2.75 2.75 0 0 1 22.75 10v10A2.75 2.75 0 0 1 20 22.75H10A2.75 2.75 0 0 1 7.25 20V10A2.75 2.75 0 0 1 10 7.25zm-10 1.5c-.69 0-1.25.56-1.25 1.25v10c0 .69.56 1.25 1.25 1.25h10c.69 0 1.25-.56 1.25-1.25V10c0-.69-.56-1.25-1.25-1.25z",clipRule:"evenodd"}),(0,t.jsx)("path",{d:"M14 1.25A2.756 2.756 0 0 1 16.75 4a.75.75 0 0 1-1.5 0c0-.686-.564-1.25-1.25-1.25H4c-.686 0-1.25.564-1.25 1.25v10c0 .686.564 1.25 1.25 1.25a.75.75 0 0 1 0 1.5A2.756 2.756 0 0 1 1.25 14V4A2.756 2.756 0 0 1 4 1.25z"})]})}])},706323,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 1.25a.75.75 0 0 1 .75.75v2.25H17a.75.75 0 0 1 0 1.5h-4.25v5.5h1.75a4.25 4.25 0 0 1 0 8.5h-1.75V22a.75.75 0 0 1-1.5 0v-2.25H6a.75.75 0 0 1 0-1.5h5.25v-5.5H9.5a4.25 4.25 0 0 1-4.245-4.04L5.25 8.5A4.25 4.25 0 0 1 9.5 4.25h1.75V2a.75.75 0 0 1 .75-.75m.75 17h1.75l.271-.014a2.749 2.749 0 0 0 0-5.472l-.271-.014h-1.75zM9.5 5.75A2.75 2.75 0 0 0 6.75 8.5l.014.271A2.75 2.75 0 0 0 9.5 11.25h1.75v-5.5z",clipRule:"evenodd"})})}])},936706,e=>{"use strict";var t=e.i(276385),i=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(i.default,{...e,children:(0,t.jsx)("path",{d:"M0 .21h7.098l6.561 9.167L21.826 0l1.94.035-9.14 10.66L24 23.79h-7.095l-6.193-8.543L3.147 24h-1.91l8.536-10.001zm6.429 1.354H2.68L17.678 22.4h3.681z"})})}])},791729,e=>{"use strict";var t=e.i(968323);e.s(["isEligibleForUsageExplanationDetail",0,{NEEDS_PAYMENT_METHOD:"A payment method is required. Navigate to Account > Billing to resolve.",NEEDS_SUBSCRIPTION:"A subscription is required. Navigate to Account > Billing to resolve.",NEEDS_SUBSCRIPTION_OR_PAYMENT_METHOD:"Either a subscription or payment method is required. Navigate to Account > Billing to resolve.",NEEDS_SMS_VERIFICATION:"A verified phone number is required. Go to Settings > Profile and select Verify under Your phone number to resolve.",NEEDS_UNBANNING:"Usage-based services are unavailable for this account. Please contact support@replit.com for assistance.",CREDIT_DEPLETED:"You've used all your credits. Add credits in Billing to continue.",FREE_TIER_CLOUD_BUDGET_EXCEEDED:"You've used your monthly free cloud budget. Upgrade your plan to continue now, or wait for your monthly budget to reset.",INCLUDED_IN_SUBSCRIPTION:"Plan found.",HAS_PAYMENT_METHOD:"Payment method found.",INSUFFICIENT_BUDGET:"You've reached your monthly usage budget. Navigate to Account > Billing to increase your budget.",PAYMENT_DELINQUENT:"Your payment is past due. Pay outstanding invoices or update your payment method in Billing to continue.",ENTERPRISE_EXEMPTION:"Enterprise deal orgs are exempt from suspension, banning, and payment method requirements.",USER_USAGE_ALERT_THRESHOLD_EXCEEDED:"You have reached your team's usage budget. Request a budget increase from your team admin.",GROUP_USAGE_ALERT_THRESHOLD_EXCEEDED:"You have reached your group's usage budget. Request a budget increase from your team admin."},"validateAlertThresholds",0,function(e,i){let a,r;return(null!==e&&e<.01&&(a="Usage alert value must be at least 0.01 or unset."),null!==i&&i<.01&&(r="Usage limit value must be at least 0.01 or unset."),null!==e&&null!==i&&i<=e&&(a=`Usage alert ($${e}) must be less than the usage budget ($${i}).`,r=`Usage budget ($${i}) must be greater than the usage alert ($${e}).`),a||r)?(0,t.Err)({softAlertError:a,hardAlertError:r}):(0,t.Ok)({softAlertValue:e,hardAlertValue:i})}])},521593,e=>{"use strict";let t={EXPIRY_MONTHS:6,PACKS:[{id:"credit_pack_100",credits:100,priceUsd:100},{id:"credit_pack_300",credits:300,priceUsd:290},{id:"credit_pack_500",credits:500,priceUsd:480},{id:"credit_pack_1000",credits:1e3,priceUsd:950},{id:"credit_pack_5",credits:5,priceUsd:5,hiddenFromMainDropdown:!0},{id:"credit_pack_10",credits:10,priceUsd:10,hiddenFromMainDropdown:!0},{id:"credit_pack_25",credits:25,priceUsd:25,hiddenFromMainDropdown:!0}]},i=t.PACKS.map(e=>e.id);e.s(["CREDIT_PACK_CONFIG",0,t,"VALID_PACK_IDS",0,i,"getPackById",0,function(e){return t.PACKS.find(t=>t.id===e)??null},"isPackHiddenFromMainDropdown",0,function(e){return"hiddenFromMainDropdown"in e&&e.hiddenFromMainDropdown},"isValidPackId",0,function(e){return i.includes(e)}])}]);

//# debugId=d34501b7-c85b-3163-4719-fc4a94850df8
//# sourceMappingURL=399rv0v613oz8.js.map