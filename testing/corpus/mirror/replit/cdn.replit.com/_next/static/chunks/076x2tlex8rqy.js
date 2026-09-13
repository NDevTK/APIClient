;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="f73d4907-0f49-6ba7-02c1-8026997011ce")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,662297,e=>{"use strict";var t=e.i(807988),r=e.i(569910),i=e.i(709485),a=e.i(415541),n=e.i(987520),s=e.i(313287);let o={[t.FileOutputType.FILE_OUTPUT_TYPE_AUDIO]:"audio",[t.FileOutputType.FILE_OUTPUT_TYPE_CHART]:"html",[t.FileOutputType.FILE_OUTPUT_TYPE_HTML]:"html",[t.FileOutputType.FILE_OUTPUT_TYPE_IMAGE]:"image",[t.FileOutputType.FILE_OUTPUT_TYPE_PDF]:"pdf",[t.FileOutputType.FILE_OUTPUT_TYPE_SLIDES]:"slides",[t.FileOutputType.FILE_OUTPUT_TYPE_TABLE]:"spreadsheet",[t.FileOutputType.FILE_OUTPUT_TYPE_TEXT]:"text",[t.FileOutputType.FILE_OUTPUT_TYPE_UNSPECIFIED]:"unknown",[t.FileOutputType.FILE_OUTPUT_TYPE_VIDEO]:"video",[t.FileOutputType.FILE_OUTPUT_TYPE_WORD_DOCUMENT]:"document"};function l(e){return"keydown"===e.type?"keyboard":"pointer"}e.s(["interactionMethod",0,l,"trackGlobalSearchResourceSelection",0,function(e,{event:t,filter:u,position:d,searchSessionId:c,source:p}){let g={filter:u,interaction_method:l(t),result_position:d,search_session_id:c};switch(e.__typename){case"OrgSearchReplResult":(0,a.trackV2)(i.eventsV2.GLOBAL_SEARCH_USED,{action:"resource_selected",...g,...p,resource_id:e.repl.id,resource_type:"project"});return;case"OrgSearchConversationResult":(0,a.trackV2)(i.eventsV2.GLOBAL_SEARCH_USED,{action:"resource_selected",...g,...p,resource_id:e.conversation.id,resource_type:"chat"});return;case"OrgSearchAssetResult":var m,h;let f;(0,a.trackV2)(i.eventsV2.GLOBAL_SEARCH_USED,{action:"resource_selected",...g,...p,file_type:(m=e.asset.displayName,h=e.asset.contentType,f=(0,s.fileTypeFromContentType)(h??""),(0,s.isHtmlFile)(f,m,h)?"html":o[f]),resource_id:e.asset.id,resource_type:"file"});return;case"OrgSearchArtifactResult":(0,a.trackV2)(i.eventsV2.GLOBAL_SEARCH_USED,{action:"resource_selected",...g,...p,artifact_type:n.ARTIFACT_KIND_BY_TYPE[e.artifact.artifactType],resource_id:e.artifact.id,resource_type:"artifact"});return;default:(0,r.default)(e)}}])},447963,e=>{"use strict";var t=e.i(389959),r=e.i(960933),i=e.i(621738),a=e.i(618876),n=e.i(987997),s=e.i(489859);let o={creation:"creation-agent-mode",home:"home-agent-mode"},l=r.Type.Union([r.Type.Literal("free"),r.Type.Literal("lite"),r.Type.Literal("economy"),r.Type.Literal("power")]),u="home-agent-auto-mode";function d(e){return s.default.get((0,n.creationAgentStorageKey)(u,e),r.Type.Boolean())}function c(e,t){return s.default.get((0,n.creationAgentStorageKey)(o[e],t),l)}function p(e,t,r){s.default.set((0,n.creationAgentStorageKey)(o[e],r),t)}function g(e){switch(e){case"free":return"FREE";case"lite":return"LITE";case"power":return"POWER";case"economy":return"ECONOMY";default:return"UNSPECIFIED"}}function m(e){switch(e){case"FREE":return"free";case"LITE":return"lite";case"POWER":case"TURBO":return"power";default:return"economy"}}function h(e,t,r,{ignoreStoredTier:n=!1}={}){return{...i.DEFAULT_CHATEAU_AGENT_CONFIG,modelProfile:n?"UNSPECIFIED":g(c("home",r)),intelligentAutoMode:d(r)??void 0,...(0,a.filterCreationAgentModelPicks)((0,a.readCreationAgentModelPicks)(e,r),t)}}e.s(["modelProfileForTier",0,g,"readLocalAgentConfig",0,h,"readStoredAutoMode",0,d,"readStoredTier",0,c,"tierForModelProfile",0,m,"useLocalAgentConfigPersistence",0,function(e,{areSettingsLocked:r,ignoreStoredTier:o=!1,isIntelligentAutoModeAvailable:l,optionsByUIMode:d,orgId:c,skip:g}){let[f,b]=(0,t.useState)(!1);return(0,t.useEffect)(()=>b(!0),[]),{initialConfig:(0,t.useMemo)(()=>{if(!f||g)return;if(r)return i.DEFAULT_CHATEAU_AGENT_CONFIG;let t=h(e,d,c,{ignoreStoredTier:o});return void 0===t.intelligentAutoMode&&l?{...t,intelligentAutoMode:!0}:t},[r,f,o,l,d,c,e,g]),persistConfig:(0,t.useCallback)(t=>{var r;void 0!==t.modelProfile&&"UNSPECIFIED"!==t.modelProfile&&p("home",m(t.modelProfile),c),void 0!==t.intelligentAutoMode&&(r=t.intelligentAutoMode,s.default.set((0,n.creationAgentStorageKey)(u,c),r)),(0,a.writeCreationAgentModelPicks)((0,a.applyCreationAgentModelPicks)((0,a.readCreationAgentModelPicks)(e,c),t),e,c)},[c,e]),loading:!f||g}},"writeStoredTier",0,p])},394970,e=>{"use strict";var t=e.i(389959),r=e.i(908796),i=e.i(215515);function a(e,t){return t===r.OrgAuthorizationCode.RequiresActiveSubscription||void 0!==e&&t===r.OrgAuthorizationCode.InsufficientPermissions}e.s(["areAccountAgentSettingsLocked",0,a,"useAccountAgentConfigEnv",0,function(e,n,s){let{isLoading:o,isPaidUser:l,paidAgentAuthorizationCode:u,arePromotionsEnabled:d,isDefaultAdvancedAgentModelAuthorized:c,isEffortCapped:p,isFreeTierCreationEligible:g,isPerTierAutoModeAuthorized:m,isIntelligentAutoModeAuthorized:h,isFreeModeTierAuthorized:f,canEditWorkspaceSettings:b,refresh:y}=(0,i.useConversationAgentAuthorizations)(e,s),S=!o&&f,v=!l&&g,_=u===r.OrgAuthorizationCode.RequiresActiveSubscription,R=a(e,u),C=_&&d;return{env:(0,t.useMemo)(()=>({stackBlueprint:void 0,isAutomatedTestingAvailable:!1,isDefaultAdvancedAuthorized:c,isTurboAvailable:!1,isLiteAvailable:!0,isFreeDefaultLite:v,isFreeModelProfileEnabled:S,isFreeTierDefault:!0,canUpgradeToTurbo:void 0===e,isHighEffortAvailable:!1,isEffortCapped:p,isFreemiumExperience:C,isWebDesignMockup:!1,isDesignSettingsMode:"design"===n,canConfigureAgentModelSettings:!R,canUpgradeAgentModelSettings:C,isPerTierAutoModeAvailable:m,isIntelligentAutoModeAvailable:h,isAutoPublishToggleVisible:!1,canEditWorkspaceSettings:b}),[R,c,p,v,S,C,m,h,b,e,n]),isEnvLoading:o,isModeLabelEnvLoading:o,areSettingsLocked:R,refreshAuthorizations:y}}])},881258,e=>{"use strict";var t=e.i(351623),r=e.i(721037),i=e.i(299020);let a={},n=t.gql`
    mutation CarryAgentConfigToRepl($input: UpdateCurrentUserReplSettingsInput!) {
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
    ${r.UserReplSettingsFragmentFragmentDoc}`;e.s(["useCarryAgentConfigToReplMutation",0,function(e){let t={...a,...e};return i.useMutation(n,t)}])},18109,e=>{"use strict";var t=e.i(389959),r=e.i(881258),i=e.i(328789),a=e.i(429843),n=e.i(577671);e.s(["useCarryAgentConfigToRepl",0,function(){let[e]=(0,r.useCarryAgentConfigToReplMutation)();return(0,t.useCallback)((t,r)=>(0,i.persistCreationModePickerSettings)({replId:t,modelProfile:r.modelProfile,isModelProfileChosen:"UNSPECIFIED"!==r.modelProfile,isPlanModeEnabled:!1,persist:async(t,a,s)=>{let{data:o}=await e({context:{noBatch:!0,timeoutMs:i.PERSIST_AGENT_MODE_SETTINGS_TIMEOUT_MS},variables:{input:{replId:t,agentSettings:(0,i.getCreationModePickerAgentSettings)({modelTier:a,isPlanModeEnabled:s,modelPicks:r.modelPicks,modelSettingsMode:"build",intelligentAutoMode:r.intelligentAutoMode})}},update:(e,r)=>{let i=r.data?.updateCurrentUserReplSettings;i?.__typename==="UpdateCurrentUserReplSettingsPayload"&&(0,n.writeCurrentUserReplSettingsToCache)({cache:e,replId:t,settings:i.userReplSettings})}}),l=o?.updateCurrentUserReplSettings;if(l?.__typename==="UpdateCurrentUserReplSettingsPayload")return"persisted";if(l?.__typename==="NotFoundError")return"repl-not-found";throw Error(l&&"message"in l?l.message:"Could not save the Agent mode")},onError:e=>a.logger.error("failed to carry the Agent mode",{replid:t,reason:e.message})}),[e])}])},101626,e=>{"use strict";var t=e.i(351623),r=e.i(344480),i=e.i(975473);let a={},n=t.gql`
    fragment ConversationAgentAuthorizations on OrgAuthorizations {
  __typename
  paidAgent: useAiAgent(tier: paid) {
    isAuthorized
    code
  }
  highEffortAgentModel: useHighEffort {
    isAuthorized
  }
  defaultAdvancedAgentModel: defaultAdvancedAgentModel {
    isAuthorized
  }
  createReplWithLiteMode {
    isAuthorized
  }
  perTierAutoMode: useAgentConfigPerTierAutoMode {
    isAuthorized
  }
  intelligentAutoMode: useAgentConfigIntelligentAutoMode {
    isAuthorized
  }
  freeModeTier: useAgentConfigFreeModeTier {
    isAuthorized
  }
  editSettings {
    isAuthorized
  }
}
    `,s=t.gql`
    query ConversationAgentAuthorizations($orgId: String!, $hasOrg: Boolean!) {
  currentUser @skip(if: $hasOrg) {
    id
    customer {
      __typename
      ... on Customer {
        id
        authorizations {
          enablePromotions {
            isAuthorized
          }
        }
      }
    }
    personalOrgAuthorizations {
      __typename
      ...ConversationAgentAuthorizations
    }
  }
  getOrg(orgId: $orgId) @include(if: $hasOrg) {
    __typename
    ... on Org {
      id
      customer {
        __typename
        ... on Customer {
          id
          authorizations {
            enablePromotions {
              isAuthorized
            }
          }
        }
      }
      authorizations {
        ...ConversationAgentAuthorizations
      }
    }
  }
}
    ${n}`;e.s(["useConversationAgentAuthorizationsLazyQuery",0,function(e){let t={...a,...e};return i.useLazyQuery(s,t)},"useConversationAgentAuthorizationsQuery",0,function(e){let t={...a,...e};return r.useQuery(s,t)}])},215515,e=>{"use strict";var t=e.i(389959),r=e.i(101626);function i(e){return{orgId:e??"",hasOrg:void 0!==e}}function a(e){let t,r=e?.getOrg,i=e?.currentUser?.personalOrgAuthorizations,a=r?.__typename==="Org"?r.customer:e?.currentUser?.customer;return r?.__typename==="Org"?t=r.authorizations:i?.__typename==="OrgAuthorizations"&&(t=i),{isPaidUser:t?.paidAgent?.isAuthorized??!1,paidAgentAuthorizationCode:t?.paidAgent?.code,arePromotionsEnabled:a?.__typename==="Customer"&&a.authorizations.enablePromotions.isAuthorized,isFreeTierCreationEligible:t?.createReplWithLiteMode?.isAuthorized??!1,isFreeModeTierAuthorized:t?.freeModeTier?.isAuthorized??!1,isDefaultAdvancedAgentModelAuthorized:t?.defaultAdvancedAgentModel?.isAuthorized??!1,isEffortCapped:t?.highEffortAgentModel?.isAuthorized!==!0,isPerTierAutoModeAuthorized:t?.perTierAutoMode?.isAuthorized??!1,isIntelligentAutoModeAuthorized:t?.intelligentAutoMode?.isAuthorized??!1,canEditWorkspaceSettings:t?.editSettings?.isAuthorized??!1}}e.s(["useConversationAgentAuthorizations",0,function(e,n=!1){let{data:s,loading:o,refetch:l}=(0,r.useConversationAgentAuthorizationsQuery)({variables:i(e),skip:n}),u=(0,t.useCallback)(()=>{l()},[l]);return{isLoading:n||o,...a(s),refresh:u}},"useLazyConversationAgentAuthorizations",0,function(e){let[n]=(0,r.useConversationAgentAuthorizationsLazyQuery)({fetchPolicy:"cache-first"});return(0,t.useCallback)(async()=>{let{data:t}=await n({variables:i(e),context:{timeoutMs:2e3}});return a(t)},[n,e])}])},820669,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment CollaboratorCountV2Customer on Customer {
  id
  seats {
    __typename
    ... on CustomerSeats {
      id
      counts {
        admin
        guest
        member
      }
      caps {
        members
      }
    }
  }
  subscriptionSummary {
    __typename
    ... on CustomerSubscriptionSummarySelfServe {
      plan {
        ... on CustomerSubscriptionSummaryFlatSelfServePlan {
          planPrefix
        }
        ... on CustomerSubscriptionSummaryTieredSelfServePlan {
          planPrefix
        }
      }
    }
  }
}
    `;e.s(["CollaboratorCountV2CustomerFragmentDoc",0,r])},665061,e=>{"use strict";e.s(["commandDefaultScores",0,{findOrCreateFile:1,searchFileContents:2,fork:.5,createNewRepl:.5,askAssistant:.5,settings:.5},"commandMatchScoreMultipliers",0,{files:.98,settings:.9,toolDescription:.8,askAssistant:.5,fileContentsSearch:.4}])},558407,e=>{"use strict";var t=e.i(276385),r=e.i(389959),i=e.i(602351),a=e.i(19777),n=e.i(485792),s=e.i(109591),o=e.i(871579),l=e.i(709485),u=e.i(217403);e.i(214847);var d=e.i(864300),c=e.i(415541);e.i(119474);var p=e.i(23818),g=e.i(295621),m=e.i(744093),h=e.i(89148);let f="project-actions",b=(0,g.defaultKeyCombo)({cmdOrCtrl:!0,key:"k"});function y(e,t){(0,c.trackV2)(l.eventsV2.GLOBAL_SEARCH_USED,{action:"opened",entry_point:e,search_session_id:t})}function S(e,t){(0,c.trackV2)(l.eventsV2.GLOBAL_SEARCH_USED,{action:"search_ended",reason:e,search_session_id:t})}e.s(["PROJECT_ACTIONS_COMMAND_KEY",0,f,"defaultGlobalSearchPaletteShortcut",0,b,"useCloseGlobalSearchPalette",0,function(){let e=(0,a.useSetAtom)(m.closeGlobalSearchPaletteAtom);return(0,r.useCallback)(t=>{let r=e();null!==r&&S(t,r)},[e])},"useGlobalSearchPaletteFilter",0,function(){return[(0,a.useAtomValue)(m.globalSearchPaletteFilterAtom),(0,a.useSetAtom)(m.globalSearchPaletteFilterAtom)]},"useGlobalSearchPaletteIsOpen",0,function(){return(0,a.useAtomValue)(m.globalSearchPaletteIsOpenAtom)},"useGlobalSearchPalettePresentation",0,function(){return(0,a.useAtomValue)(m.globalSearchPalettePresentationAtom)},"useGlobalSearchPaletteUsesRootPresentation",0,function(){return(0,a.useAtomValue)(m.globalSearchPaletteUsesRootPresentationAtom)},"useGlobalSearchSessionId",0,function(){return(0,a.useAtomValue)(m.globalSearchPaletteSessionIdAtom)},"useInitialCommand",0,function(){let e,r,l,c,g=(e=(0,p.useKeyComboWithFallback)("toggleGlobalCommandBarCommand",b),r=(0,u.default)(()=>({type:"context",icon:(0,t.jsx)(o.default,{}),label:"",description:"Search…",keyboardShortcut:e}),[e]),l=(0,d.useIntl)().formatMessage({id:"components.commandPalette.actions",defaultMessage:"Actions"}),c=(0,u.default)(()=>(0,n.selectAtom)((0,i.atom)(e=>{let t=e(m.globalSearchPaletteFilterAtom),r=e(m.globalSearchPalettePresentationAtom).groupProjectCommands;return e(m.commandsAtom).filter(({scope:e})=>"all"===t?!r||"global"===e:"project"===t||"clui"===t?e===t:"global"===e)}),e=>e,(e,t)=>e.length===t.length&&e.every((e,r)=>e===t[r])),[]),(0,u.default)(()=>(0,i.atom)(e=>{let i=e(c),a=e(m.globalSearchPalettePresentationAtom).groupProjectCommands;return{data:r,commands:()=>{let e=[...i];if(!a)return e.sort((e,t)=>t.priority-e.priority).map(({command:e})=>e);let r=e.filter(({scope:e})=>"project"===e),n=e.filter(({scope:e})=>"project"!==e);return r.length>0&&n.push({command:{data:{type:"context",label:l,icon:(0,t.jsx)(t.Fragment,{})},commands:()=>r.sort((e,t)=>t.priority-e.priority).map(({command:e})=>e)},priority:Math.max(...r.map(({priority:e})=>e)),scope:"project"}),n.sort((e,t)=>t.priority-e.priority).map(({command:e})=>e)}}}),[l,c,r])),y=(0,u.default)(()=>(0,i.atom)(e=>{let r=e(g),i=e(m.initialSubCommandAtom);if(i)return[r,i];let a=e(m.projectCommandLabelAtom),n=e(m.commandsAtom).filter(({scope:e})=>"project"===e||"clui"===e);return"project"===e(m.globalSearchPaletteFilterAtom)&&a&&0!==n.length?[r,{data:{type:"context",key:f,label:a,icon:(0,t.jsx)(s.default,{size:16,color:h.tokens.foregroundDimmest})},match:()=>({score:1}),commands:()=>n.sort((e,t)=>t.priority-e.priority).map(({command:e})=>e)}]:r}),[g]);return(0,a.useAtomValue)(y)},"useOpenGlobalSearchPalette",0,function(e){let t=(0,a.useSetAtom)(m.openGlobalSearchPaletteAtom);return(0,r.useCallback)((r={})=>{let i=t(r);null!==i&&y(e,i)},[e,t])},"useProjectCommandLabel",0,function(){return(0,a.useAtomValue)(m.projectCommandLabelAtom)},"useSetCommands",0,function(){return(0,a.useSetAtom)(m.commandsAtom)},"useSetGlobalSearchPalettePresentation",0,function(){return(0,a.useSetAtom)(m.globalSearchPalettePresentationAtom)},"useSetGlobalSearchPaletteUsesRootPresentation",0,function(){return(0,a.useSetAtom)(m.globalSearchPaletteUsesRootPresentationAtom)},"useSetProjectCommandLabel",0,function(){return(0,a.useSetAtom)(m.projectCommandLabelAtom)},"useSetWorkspaceGlobalSearchPaletteHostActive",0,function(){return(0,a.useSetAtom)(m.workspaceGlobalSearchPaletteHostActiveAtom)},"useToggleGlobalSearchPalette",0,function(e){let t=(0,a.useSetAtom)(m.toggleGlobalSearchPaletteAtom);return(0,r.useCallback)(()=>{let r=t();null!==r.sessionId&&(r.opened?y(e,r.sessionId):S("dismissed",r.sessionId))},[e,t])},"useWorkspaceGlobalSearchPaletteHostActive",0,function(){return(0,a.useAtomValue)(m.workspaceGlobalSearchPaletteHostActiveAtom)}])},133803,e=>{e.v({surface:"index-module__jpp0Ya__surface"})},856919,e=>{"use strict";var t=e.i(276385),r=e.i(389959);e.i(214847);var i=e.i(864300);e.i(119474);var a=e.i(23818),n=e.i(558407),s=e.i(919073),o=e.i(528326),l=e.i(678852),u=e.i(133803);let d={style:{overflow:"hidden"}},c={id:"toggleGlobalCommandBarCommand",name:"Toggle global search palette",description:"Opens or closes the global search palette",default:n.defaultGlobalSearchPaletteShortcut},p=(0,r.createContext)("global"),g=(0,r.memo)(function(){let e=(0,i.useIntl)(),p=(0,n.useGlobalSearchPaletteIsOpen)(),g=(0,n.useGlobalSearchPaletteUsesRootPresentation)(),m=(0,n.useSetGlobalSearchPaletteUsesRootPresentation)(),h=(0,n.useGlobalSearchPalettePresentation)(),f=!!g&&(h.stackDescriptions??!1),b=(0,n.useCloseGlobalSearchPalette)(),y=(0,n.useInitialCommand)(),S=(0,a.useKeyComboWithFallback)(c.id,n.defaultGlobalSearchPaletteShortcut),v=(0,r.useCallback)(e=>{let t=e[e.length-1],r=t?.data.type==="context"&&t.data.key===n.PROJECT_ACTIONS_COMMAND_KEY;m(1===e.length||r)},[m]);return(0,t.jsx)(o.Modal,{hideCloseButton:!0,isOpen:p,onRequestClose:()=>b("dismissed"),noPadding:!0,skipTransition:!0,topPadding:112,underlayProps:d,children:(0,t.jsx)(s.ShadesSurface,{clsx:u.default.surface,elevate:!1,br:16,children:(0,t.jsx)(l.CommandBar,{keyboardShortcut:S,onAction:()=>b("selected"),command:y,fixedHeaderFooter:g,inputBarBottomPadding:!g,footer:g?h.footer:null,searchLoadingSkeletonCount:8*!!g,searchLoadingSkeletonLabel:g?e.formatMessage({id:"home.searchYourWork",defaultMessage:"Your work"}):void 0,subheader:g?h.subheader:null,onInputValueChange:g?h.onInputValueChange:void 0,onPathChange:v,showToolDescriptions:f,stackDescriptions:f,autoFocus:!0})})})});e.s(["GlobalSearchPaletteRegistrationScope",0,function({children:e,scope:i,projectLabel:a}){let s=(0,n.useSetProjectCommandLabel)();return(0,r.useEffect)(()=>{if("project"===i&&a)return s(a),()=>s(null)},[a,i,s]),(0,t.jsx)(p.Provider,{value:i,children:e})},"default",0,g,"toggleGlobalSearchPaletteCommand",0,c,"useCommand",0,function(e,{enabled:t=!0,priority:i=0,scope:a}={}){let s=(0,n.useSetCommands)(),o=(0,r.useContext)(p),l=a??o;(0,r.useEffect)(()=>{if(!t||null===e)return;let r={command:e,priority:i,scope:l};return s(e=>[...e,r]),()=>{s(e=>e.filter(e=>e!==r))}},[l,s,e,t,i])}])},744093,e=>{"use strict";var t=e.i(971131),r=e.i(602351),i=e.i(912206);let a=(0,r.atom)(null),n=(0,r.atom)(e=>null!==e(a)),s=(0,r.atom)(null),o=(0,r.atom)(!0),l=(0,r.atom)("all"),u=(0,r.atom)(null),d=(0,r.atom)(!1),c=(0,r.atom)(null,(e,t,r={})=>{let n=e(a)?null:(0,i.v4)(),d="subCommand"in r?r.subCommand:null,c="subCommand"in r?null:r.view??"contextual";return t(s,d),t(o,null===d),null!==n&&t(a,n),null!==c&&t(l,"contextual"===c&&e(u)?"project":"all"),n}),p=(0,r.atom)(null,(e,r)=>{let i=e(a);return(0,t.flushSync)(()=>{r(s,null),r(o,!0),r(a,null)}),i}),g=(0,r.atom)(null,(e,t)=>e(n)?{opened:!1,sessionId:t(p)}:{opened:!0,sessionId:t(c,{})}),m=(0,r.atom)([]),h=(0,r.atom)({});e.s(["closeGlobalSearchPaletteAtom",0,p,"commandsAtom",0,m,"globalSearchPaletteFilterAtom",0,l,"globalSearchPaletteIsOpenAtom",0,n,"globalSearchPalettePresentationAtom",0,h,"globalSearchPaletteSessionIdAtom",0,a,"globalSearchPaletteUsesRootPresentationAtom",0,o,"initialSubCommandAtom",0,s,"openGlobalSearchPaletteAtom",0,c,"projectCommandLabelAtom",0,u,"toggleGlobalSearchPaletteAtom",0,g,"workspaceGlobalSearchPaletteHostActiveAtom",0,d])},924596,e=>{"use strict";var t=e.i(276385),r=e.i(389959);e.i(242933);var i=e.i(790164);e.i(925218);var a=e.i(267103),n=e.i(208018);let s=(0,r.createContext)(null);e.s(["ConversationFeedbackTargetProvider",0,function({children:e}){let[a]=(0,r.useState)(()=>new i.ObservableState(null));return(0,t.jsx)(s.Provider,{value:a,children:e})},"useConversationFeedbackTarget",0,function(e){let t=(0,r.useContext)(s),i=(0,a.useObservable)(t,null);return i?.conversationId===e?i:null},"usePublishConversationFeedbackTarget",0,function(e){let t=(0,r.useContext)(s),i=e?.id??null,a=e?.orgId??null;(0,n.default)(()=>{if(null===t)return;let e=null!==i&&null!==a?{kind:"conversation",conversationId:i,orgId:a}:null;return t.set(e),()=>{t.current===e&&t.set(null)}},[i,a,t])}])},923537,e=>{"use strict";var t=e.i(276385),r=e.i(389959);let i=(0,r.createContext)(null);e.s(["CreateComposerFocusProvider",0,function({children:e}){let a=(0,r.useRef)(null);return(0,t.jsx)(i.Provider,{value:a,children:e})},"useFocusCreateComposer",0,function(){let e=(0,r.useContext)(i);return(0,r.useCallback)(()=>e?.current?.(),[e])},"useRegisterCreateComposerFocus",0,function(e){let t=(0,r.useContext)(i);(0,r.useEffect)(()=>{if(null!==t&&null!==e)return t.current=e,()=>{t.current=null}},[e,t])}])},185860,e=>{e.v({card:"CreateOutputKindHoverCard-module__sh5pla__card"})},962915,e=>{"use strict";var t=e.i(276385),r=e.i(562203),i=e.i(389959),a=e.i(625251),n=e.i(807988),s=e.i(304151),o=e.i(252204),l=e.i(572599),u=e.i(814176),d=e.i(436298),c=e.i(780902),p=e.i(62624);e.i(214847);var g=e.i(864300),m=e.i(280810),h=e.i(535230),f=e.i(965531),b=e.i(295231),y=e.i(61732),S=e.i(185860);let v=(0,d.getOutputKindConfig)((0,d.artifactKindOption)(n.ArtifactKind.ARTIFACT_KIND_DESIGN)).Icon,_=[(0,d.artifactKindOption)(n.ArtifactKind.ARTIFACT_KIND_WEB),(0,d.artifactKindOption)(n.ArtifactKind.ARTIFACT_KIND_MOBILE),(0,d.artifactKindOption)(n.ArtifactKind.ARTIFACT_KIND_SLIDES),(0,d.artifactKindOption)(n.ArtifactKind.ARTIFACT_KIND_VIDEO)],R=[(0,d.artifactKindOption)(n.ArtifactKind.ARTIFACT_KIND_DATA_VISUALIZATION),(0,d.assetKindOption)(n.AssetRequestKind.OTHER_OUTPUT_KIND_DOCUMENT),(0,d.assetKindOption)(n.AssetRequestKind.OTHER_OUTPUT_KIND_SPREADSHEET)];function C(e,t){let r=new Set(t.map(d.outputKindKey));return e.filter(e=>r.has((0,d.outputKindKey)(e)))}function T({onSelect:e,projectActions:r}){let i=(0,g.useIntl)(),n=(0,p.useMobileOutputOptions)(),d=C(_,n),c=C(R,n);return(0,t.jsx)(y.View,{clsx:S.default.card,children:(0,t.jsxs)(b.Menu,{"aria-label":i.formatMessage({id:"home.globalSidebarCreateMenuAriaLabel",defaultMessage:"Create something new"}),children:[(0,t.jsx)(b.MenuItem,{label:i.formatMessage({id:"home.globalSidebarChat",defaultMessage:"Chat"}),icon:(0,t.jsx)(u.default,{}),onAction:r.onSelectChat}),(0,t.jsx)(b.Separator,{}),(0,t.jsx)(a.MenuSection,{children:(0,t.jsx)(m.NewArtifactPickerMenuItems,{kinds:d,onSelect:e})}),(0,t.jsx)(b.Separator,{}),(0,t.jsx)(a.MenuSection,{children:(0,t.jsx)(m.NewArtifactPickerMenuItems,{kinds:c,onSelect:e})}),r.onCreateBlankDesign||r.onCreateEmptyProject||r.onImport?(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(b.Separator,{}),(0,t.jsxs)(a.MenuSection,{children:[r.onCreateBlankDesign?(0,t.jsx)(b.MenuItem,{label:i.formatMessage({id:"home.globalSidebarDesignProject",defaultMessage:"Design project"}),icon:(0,t.jsx)(v,{}),iconRight:(0,t.jsx)(o.default,{}),onAction:r.onCreateBlankDesign}):null,r.onCreateEmptyProject?(0,t.jsx)(b.MenuItem,{label:i.formatMessage({id:"home.globalSidebarEmptyProject",defaultMessage:"Empty project"}),icon:(0,t.jsx)(l.default,{}),iconRight:(0,t.jsx)(o.default,{}),onAction:r.onCreateEmptyProject}):null,r.onImport?(0,t.jsx)(b.MenuItem,{label:i.formatMessage({id:"home.globalSidebarImport",defaultMessage:"Import"}),icon:(0,t.jsx)(s.default,{}),iconRight:(0,t.jsx)(o.default,{}),href:"/import",onAction:r.onImport}):null]})]}):null]})})}e.s(["CreateOutputKindHoverCard",0,function({onSelect:e,projectActions:a,isDisabled:n=!1,children:s}){let o=(0,c.useIsMobile)(),l=(0,r.useRouter)(),[u,d]=(0,i.useState)(!1);(0,h.useReportSidebarInteractionOpen)(u);let p=(0,i.useRef)(!1),[g,m]=(0,i.useState)(0);return(0,i.useEffect)(()=>{if(!l)return;let e=()=>{p.current&&(p.current=!1,d(!1),m(e=>e+1))};return l.events.on("routeChangeStart",e),()=>{l.events.off("routeChangeStart",e)}},[l]),(0,t.jsx)(f.HoverCard,{isDisabled:o||n,openDelayMs:500,placement:"right top",offset:4,opensOnFocus:!1,content:(0,t.jsx)(T,{onSelect:e,projectActions:a}),onOpenChange:e=>{p.current=e,d(e)},children:s},g)}])},682077,e=>{"use strict";var t=e.i(276385),r=e.i(196786),i=e.i(269848);e.i(214847);var a=e.i(864300),n=e.i(643484),s=e.i(528326),o=e.i(8047),l=e.i(61732);let u=(0,r.default)(()=>e.A(529716).then(e=>e.AgentFeedback),{loadableGenerated:{modules:[760325]},ssr:!1,loading:({error:e})=>(0,t.jsx)(d,{error:e})});function d({error:e}){let r=(0,a.useIntl)();return e?(0,t.jsxs)(l.View,{p:24,gap:12,align:"center","aria-live":"polite","aria-atomic":!0,children:[(0,t.jsx)(o.Text,{variant:"small",color:"dimmer",children:r.formatMessage({id:"home.globalSidebarFeedbackFormLoadError",defaultMessage:"Could not load the feedback form."})}),(0,t.jsx)(n.Button,{text:r.formatMessage({id:"home.globalSidebarReloadPage",defaultMessage:"Reload page"}),onClick:()=>window.location.reload(),size:"small"})]}):(0,t.jsx)(l.View,{p:24,align:"center",justify:"center","aria-live":"polite","aria-atomic":!0,children:(0,t.jsx)(i.default,{alt:r.formatMessage({id:"home.globalSidebarFeedbackFormLoading",defaultMessage:"Loading feedback form"})})})}e.s(["GlobalSidebarAccountFeedbackModal",0,function({target:e,isOpen:r,onRequestClose:i,onOpenSupportTicket:a}){return(0,t.jsx)(s.Modal,{isOpen:r,onRequestClose:i,children:(0,t.jsx)(u,{target:e,onClose:i,onOpenSupportTicket:a})})}])},703415,e=>{"use strict";var t=e.i(276385),r=e.i(196786),i=e.i(15801),a=e.i(389959);e.i(925218);var n=e.i(267103);e.i(214847);var s=e.i(864300),o=e.i(114953),l=e.i(924596),u=e.i(682077),d=e.i(616188),c=e.i(970157),p=e.i(573380),g=e.i(535230),m=e.i(39642),h=e.i(773222);let f=(0,r.default)(()=>e.A(782002).then(e=>e.GlobalSidebarAccountMenuBody),{ssr:!1,loading:({error:e})=>(0,t.jsx)(p.GlobalSidebarPopoverBodyState,{error:e})});e.s(["GlobalSidebarAccountMenu",0,function({currentUser:r,notificationCount:p,onOpenNotifications:b,settingsSurface:y,environmentsHref:S,compact:v=!1}){let _=(0,s.useIntl)(),[R,C]=(0,a.useState)(!1),T=function(){let e=(0,i.useRouter)(),t=(0,n.useObservable)(o.replMainAgentTargetState,null),{conversationId:r}=e.query,a=(0,l.useConversationFeedbackTarget)("string"==typeof r?r:null);return t?.kind==="target"?{kind:"repl",agentId:t.target.agentId}:a}(),[x,I]=(0,m.useGlobalSidebarAccountModal)(T);(0,g.useReportSidebarInteractionOpen)(R||(x?.isOpen??!1)),(0,a.useEffect)(()=>{e.A(782002).catch(()=>{})},[]);let A=p>0?_.formatMessage({id:"home.globalSidebarAccountMenuUnread",defaultMessage:"Account menu, {count, plural, one {# unread notification} other {# unread notifications}}"},{count:p}):_.formatMessage({id:"home.globalSidebarAccountMenu",defaultMessage:"Account menu"});return(0,t.jsxs)(t.Fragment,{children:[(0,t.jsxs)(h.PopoverTrigger,{isOpen:R,onOpenChange:C,placement:"top start",label:A,children:[e=>(0,t.jsx)(d.GlobalSidebarAccountMenuTrigger,{currentUser:r,notificationCount:p,compact:v,label:A,triggerProps:e}),(0,t.jsx)(f,{currentUser:r,notificationCount:p,label:A,settingsSurface:y,environmentsHref:S,onClose:()=>C(!1),onOpenNotifications:()=>{C(!1),b()},onOpenSupport:()=>{C(!1),I({name:"support",isOpen:!0})},onOpenFeedback:null===T?void 0:()=>{C(!1),I({name:"feedback",isOpen:!0,target:T})}})]}),x?.name==="support"?(0,t.jsx)(c.GlobalSidebarAccountSupportModal,{isOpen:x.isOpen,onRequestClose:()=>I({name:"support",isOpen:!1})}):null,x?.name==="feedback"?(0,t.jsx)(u.GlobalSidebarAccountFeedbackModal,{target:x.target,isOpen:x.isOpen,onRequestClose:()=>I({...x,isOpen:!1}),onOpenSupportTicket:()=>I({name:"support",isOpen:!0})}):null]})}])},744297,e=>{e.v({avatar:"GlobalSidebarAccountMenuTrigger-module__tzEAOq__avatar",cardTrigger:"GlobalSidebarAccountMenuTrigger-module__tzEAOq__cardTrigger",notificationIndicator:"GlobalSidebarAccountMenuTrigger-module__tzEAOq__notificationIndicator"})},616188,e=>{"use strict";var t=e.i(276385),r=e.i(27923),i=e.i(406664),a=e.i(919073),n=e.i(825419),s=e.i(488299),o=e.i(8047),l=e.i(61732),u=e.i(744297);function d({accountName:e,image:r,notificationCount:i}){return(0,t.jsxs)(l.View,{clsx:u.default.avatar,children:[(0,t.jsx)(n.Avatar,{src:r,username:e,size:20}),i>0?(0,t.jsx)(a.ShadesSurface,{clsx:u.default.notificationIndicator,colorShade:"themeError",align:"center",justify:"center","data-global-sidebar-notification-indicator":"true","aria-hidden":!0,children:(0,t.jsx)(o.Text,{variant:"small",children:i})}):null]})}e.s(["GlobalSidebarAccountMenuTrigger",0,function({currentUser:e,notificationCount:a,compact:n=!1,label:l,triggerProps:c}){let p=e.firstName?.trim()||e.username,{ref:g,...m}=c;return(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(s.IconButton,{...m,ref:n?g:void 0,style:n?void 0:{display:"none"},alt:l,size:32,tooltipPlacement:"right","data-analytics-id":n?"global_sidebar_account_menu_button":void 0,"data-analytics-label":"account_menu",children:(0,t.jsx)(d,{accountName:p,image:e.image,notificationCount:n?a:0})}),(0,t.jsxs)(i.Interactive,{...m,innerRef:n?void 0:g,style:n?{display:"none"}:void 0,tag:"button",type:"button",variant:"nofill",borderRadius:12,clsx:u.default.cardTrigger,row:!0,grow:!0,shrink:!0,align:"center",gap:8,p:6,pr:44,"aria-label":l,"data-analytics-id":n?void 0:"global_sidebar_account_menu_button","data-analytics-label":"account_menu",children:[(0,t.jsx)(d,{accountName:p,image:e.image,notificationCount:n?0:a}),(0,t.jsx)(o.Text,{variant:"text",multiline:!1,shrink:!0,clsx:(0,r.tw)("min-w-0"),children:p})]})]})}])},970157,e=>{"use strict";var t=e.i(276385),r=e.i(196786),i=e.i(269848);e.i(214847);var a=e.i(864300),n=e.i(643484),s=e.i(528326),o=e.i(8047),l=e.i(61732);let u=(0,r.default)(()=>e.A(340261),{loadableGenerated:{modules:[233174]},ssr:!1,loading:({error:e})=>(0,t.jsx)(d,{error:e})});function d({error:e}){let r=(0,a.useIntl)();return e?(0,t.jsxs)(l.View,{p:24,gap:12,align:"center","aria-live":"polite","aria-atomic":!0,children:[(0,t.jsx)(o.Text,{variant:"small",color:"dimmer",children:r.formatMessage({id:"home.globalSidebarSupportFormLoadError",defaultMessage:"Could not load the support form."})}),(0,t.jsx)(n.Button,{text:r.formatMessage({id:"home.globalSidebarReloadPage",defaultMessage:"Reload page"}),onClick:()=>window.location.reload(),size:"small"})]}):(0,t.jsx)(l.View,{p:24,align:"center",justify:"center","aria-live":"polite","aria-atomic":!0,children:(0,t.jsx)(i.default,{alt:r.formatMessage({id:"home.globalSidebarSupportFormLoading",defaultMessage:"Loading support form"})})})}e.s(["GlobalSidebarAccountSupportModal",0,function({isOpen:e,onRequestClose:r}){return(0,t.jsx)(s.Modal,{isOpen:e,onRequestClose:r,children:(0,t.jsx)(u,{onRequestClose:r})})}])},794311,e=>{e.v({body:"GlobalSidebarPopoverBodyState-module__BK_Fsq__body"})},573380,e=>{"use strict";var t=e.i(276385),r=e.i(269848);e.i(214847);var i=e.i(864300),a=e.i(643484),n=e.i(8047),s=e.i(61732),o=e.i(794311);e.s(["GlobalSidebarPopoverBodyState",0,function({error:e}){let l=(0,i.useIntl)();return e?(0,t.jsxs)(s.View,{p:12,gap:8,align:"center",clsx:o.default.body,children:[(0,t.jsx)(n.Text,{variant:"small",color:"dimmer",children:l.formatMessage({id:"home.globalSidebarMenuLoadError",defaultMessage:"Could not load this menu."})}),(0,t.jsx)(a.Button,{text:l.formatMessage({id:"home.globalSidebarReloadPage",defaultMessage:"Reload page"}),onClick:()=>window.location.reload(),size:"small"})]}):(0,t.jsx)(s.View,{p:12,align:"center",justify:"center",clsx:o.default.body,children:(0,t.jsx)(r.default,{alt:l.formatMessage({id:"home.globalSidebarMenuLoading",defaultMessage:"Loading menu"})})})}])},39642,e=>{"use strict";var t=e.i(389959);e.s(["useGlobalSidebarAccountModal",0,function(e){let[r,i]=(0,t.useState)(null);return(0,t.useEffect)(()=>{r?.name==="feedback"&&r.isOpen&&(null===e||!function(e,t){switch(e.kind){case"repl":return"repl"===t.kind&&e.agentId===t.agentId;case"conversation":return"conversation"===t.kind&&e.conversationId===t.conversationId&&e.orgId===t.orgId;default:throw Error(`Unknown feedback target: ${e}`)}}(r.target,e))&&i({...r,isOpen:!1})},[e,r]),[r,i]}])},372090,e=>{"use strict";e.i(242933);var t=e.i(790164),r=e.i(489859),i=e.i(349892),a=e.i(127384);let n="replit.global-sidebar-width",s="replit.global-sidebar-open";function o(e,t){if(null===t)return Math.max(e,a.MIN_GLOBAL_SIDEBAR_WIDTH);let r=Math.round(t*a.MAX_GLOBAL_SIDEBAR_WIDTH_PERCENTAGE);return Math.min(Math.max(e,a.MIN_GLOBAL_SIDEBAR_WIDTH),r)}function l(e){return"desktop"!==e.viewport||0===e.layoutCount||e.suppressorCount>0?0:u(e)?e.sidebarWidth:d(e)?a.COLLAPSED_TASK_SIDEBAR_WIDTH:0}function u(e){return e.isOpen||e.forceOpenCount>0&&"desktop"===e.viewport}function d(e){return!u(e)&&!e.hasCoarsePointer&&"desktop"===e.viewport&&e.layoutCount>0&&0===e.suppressorCount}function c(e){return"desktop"!==e.viewport||0===e.layoutCount||e.suppressorCount>0||null===e.windowHeight?null:u(e)?e.sidebarWidth:d(e)?a.COLLAPSED_TASK_SIDEBAR_WIDTH:null}function p(e,t){return e.x===t.x&&e.y===t.y&&e.width===t.width&&e.height===t.height}function g(e,t){return e===t||null!==e&&null!==t&&e.x===t.x&&e.y===t.y&&e.width===t.width&&e.height===t.height}class m{state=new t.ObservableState({isOpen:!1,isTemporarilyOpen:!1,sidebarWidth:a.SIDEBAR_WIDTH,isResizing:!1,viewport:"unknown",windowWidth:null,windowHeight:null,layoutCount:0,suppressorCount:0,takeoverCount:0,forceOpenCount:0,projectListEnabled:!1,hasCoarsePointer:!1,hasTouchCapablePointer:!1,expandedReplIds:[],expandedConversationIds:[]});preferredSidebarWidth=a.SIDEBAR_WIDTH;preferredIsOpen=null;resizeStartWidth=null;resizeStartPreferredWidth=null;resizeFallbackPreferredWidth=null;layouts=new Set;suppressors=new Set;takeovers=new Set;forceOpeners=new Set;isOpen=this.state.select(u);isTemporarilyOpen=this.state.select(e=>e.isTemporarilyOpen);isVisuallyOpen=this.state.select(e=>u(e)||e.isTemporarilyOpen);sidebarWidth=this.state.select(e=>e.sidebarWidth);isResizing=this.state.select(e=>e.isResizing);viewport=this.state.select(e=>e.viewport);hasCoarsePointer=this.state.select(e=>e.hasCoarsePointer);hasTouchCapablePointer=this.state.select(e=>e.hasTouchCapablePointer);isLayoutActive=this.state.select(e=>e.layoutCount>0);contentOffset=this.state.select(l);hasContentOffset=this.state.select(e=>l(e)>0);isRail=this.state.select(d);isCollapsedToRail=this.state.select(e=>d(e)&&!e.isTemporarilyOpen);shellWidth=this.state.select(e=>d(e)&&!e.isTemporarilyOpen?a.COLLAPSED_TASK_SIDEBAR_WIDTH:e.sidebarWidth);resizeHandleRect=this.state.select(e=>{if(e.isTemporarilyOpen)return{x:0,y:0,width:0,height:0};let t=l(e);return{x:t-1,y:0,width:+(0!==t),height:e.windowHeight??0}},p);contentArea=this.state.select(e=>{if(null===e.windowWidth||null===e.windowHeight)return null;let t=l(e);return{x:t,y:0,width:e.windowWidth-t,height:e.windowHeight}},g);isShellVisible=this.state.select(e=>"unknown"!==e.viewport&&e.layoutCount>0&&0===e.suppressorCount);navMenuPlaceholderWidth=this.state.select(e=>e.layoutCount>0&&0===e.suppressorCount&&"unknown"!==e.viewport&&0===l(e)?a.COLLAPSED_TASK_SIDEBAR_WIDTH:0);isTakenOver=this.state.select(e=>e.takeoverCount>0);isProjectListEnabled=this.state.select(e=>e.projectListEnabled);expandedReplIds=this.state.select(e=>e.expandedReplIds);expandedConversationIds=this.state.select(e=>e.expandedConversationIds);toggleConversationExpanded(e){let t=this.state.current.expandedConversationIds,r=t.filter(t=>t!==e),i=r.length===t.length?[...r,e].slice(-20):r;this.patch({expandedConversationIds:i})}toggleReplExpanded(e){let t=this.state.current.expandedReplIds,r=t.filter(t=>t!==e),i=r.length===t.length?[...r,e].slice(-20):r;this.patch({expandedReplIds:i})}expandRepl(e){let t=this.state.current.expandedReplIds;if(t[t.length-1]===e)return;let r=[...t.filter(t=>t!==e),e].slice(-20);this.patch({expandedReplIds:r})}collapseRepl(e){let t=this.state.current.expandedReplIds,r=t.filter(t=>t!==e);r.length!==t.length&&this.patch({expandedReplIds:r})}collapseReplsExcept(e){let t=this.state.current.expandedReplIds,r=t.filter(t=>t===e);r.length!==t.length&&this.patch({expandedReplIds:r})}isTakeoverRegionAvailable=this.state.select(e=>null!==c(e));takeoverRegion=this.state.select(e=>{let t=c(e);return null===t||null===e.windowHeight?{x:0,y:0,width:0,height:0}:{x:0,y:a.APP_HEADER_HEIGHT,width:t,height:e.windowHeight-a.APP_HEADER_HEIGHT}},p);open(){this.isForceOpenActive()||(this.patch({isOpen:!0,isTemporarilyOpen:!1}),this.persistOpenPreference())}close(){this.isForceOpenActive()||(this.patch({isOpen:!1,isTemporarilyOpen:!1}),this.persistOpenPreference())}toggle(){this.isForceOpenActive()||(this.patch({isOpen:!this.state.current.isOpen,isTemporarilyOpen:!1}),this.persistOpenPreference())}setOpen(e){this.isForceOpenActive()||(this.patch({isOpen:e,isTemporarilyOpen:!1}),this.persistOpenPreference())}openTemporarily(){var e;d(e=this.state.current)&&!e.isResizing&&0===e.takeoverCount&&this.patch({isTemporarilyOpen:!0})}closeTemporarily(){this.state.current.isTemporarilyOpen&&this.patch({isTemporarilyOpen:!1})}persistOpenPreference(){"desktop"===this.state.current.viewport&&(this.preferredIsOpen=this.state.current.isOpen,r.default.set(s,this.preferredIsOpen))}setProjectListEnabled(e){this.state.current.projectListEnabled!==e&&this.patch({projectListEnabled:e})}setHasCoarsePointer(e){this.state.current.hasCoarsePointer!==e&&this.patch({hasCoarsePointer:e,isTemporarilyOpen:!1})}setHasTouchCapablePointer(e){this.state.current.hasTouchCapablePointer!==e&&this.patch({hasTouchCapablePointer:e})}restoreOpenState(){this.preferredIsOpen=r.default.get(s,"boolean")}restoreWidth(){let e=r.default.get(n,"number");null!==e&&Number.isFinite(e)&&(this.preferredSidebarWidth=e,this.patch({sidebarWidth:o(e,this.state.current.windowWidth)}))}startResize(){0!==l(this.state.current)&&(this.resizeFallbackPreferredWidth=this.preferredSidebarWidth,this.resizeStartWidth=u(this.state.current)?this.state.current.sidebarWidth:l(this.state.current),this.resizeStartPreferredWidth=u(this.state.current)?this.preferredSidebarWidth:this.resizeStartWidth,this.patch({isResizing:!0,isTemporarilyOpen:!1}))}resize(e){if(null===this.resizeStartWidth||null===this.resizeStartPreferredWidth)return;let t=this.resizeStartWidth+e;if(t<a.MIN_GLOBAL_SIDEBAR_WIDTH-50){this.isForceOpenActive()||this.patch({isOpen:!1,isTemporarilyOpen:!1});return}let r=o(t,this.state.current.windowWidth);this.resizeStartWidth>=this.resizeStartPreferredWidth||e<0?this.preferredSidebarWidth=r:this.preferredSidebarWidth=this.resizeStartPreferredWidth,this.patch({isOpen:!this.isForceOpenActive()||this.state.current.isOpen,isTemporarilyOpen:!1,sidebarWidth:r})}endResize(){null!==this.resizeStartWidth&&(u(this.state.current)||null===this.resizeFallbackPreferredWidth||(this.preferredSidebarWidth=this.resizeFallbackPreferredWidth,this.patch({sidebarWidth:o(this.preferredSidebarWidth,this.state.current.windowWidth)})),this.resizeStartWidth=null,this.resizeStartPreferredWidth=null,this.resizeFallbackPreferredWidth=null,this.patch({isResizing:!1}),r.default.set(n,this.preferredSidebarWidth),this.isForceOpenActive()||this.persistOpenPreference())}reset(){this.resizeStartWidth=null,this.resizeStartPreferredWidth=null,this.resizeFallbackPreferredWidth=null,this.patch({isOpen:!1,isTemporarilyOpen:!1,isResizing:!1,viewport:"unknown",windowWidth:null,windowHeight:null,projectListEnabled:!1})}addLayout(){let e=Symbol("global-sidebar-layout");return this.layouts.add(e),this.syncLayoutCount(),()=>{this.layouts.delete(e),this.syncLayoutCount()}}handleWindowResize(e,t){let r=e>=i.BREAKPOINTS.tabletMin?"desktop":"mobile",n="desktop"===r?o(this.preferredSidebarWidth,e):Math.max(Math.min(Math.round(e*a.MOBILE_GLOBAL_SIDEBAR_WIDTH_PERCENTAGE),a.MAX_MOBILE_GLOBAL_SIDEBAR_WIDTH),a.MIN_GLOBAL_SIDEBAR_WIDTH);r===this.state.current.viewport?this.patch({windowWidth:e,windowHeight:t,sidebarWidth:n}):this.patch({viewport:r,windowWidth:e,windowHeight:t,sidebarWidth:n,isOpen:"desktop"===r&&(this.preferredIsOpen??!0),isTemporarilyOpen:!1})}setViewport(e){e!==this.state.current.viewport&&this.patch({viewport:e,isOpen:"desktop"===e&&(this.preferredIsOpen??!0),isTemporarilyOpen:!1})}handleRouteChange(){"desktop"!==this.state.current.viewport&&this.patch({isOpen:!1,isTemporarilyOpen:!1})}addSuppressor(){let e=Symbol("global-sidebar-suppressor");return this.suppressors.add(e),this.syncSuppressorCount(),()=>{this.suppressors.delete(e),this.syncSuppressorCount()}}addTakeover(){let e=Symbol("global-sidebar-takeover");return this.takeovers.add(e),this.syncTakeoverCount(),()=>{this.takeovers.delete(e),this.syncTakeoverCount()}}addForceOpen(){let e=Symbol("global-sidebar-force-open");return this.forceOpeners.add(e),this.syncForceOpenCount(),()=>{this.forceOpeners.delete(e)&&this.syncForceOpenCount()}}syncSuppressorCount(){this.patch({suppressorCount:this.suppressors.size,isTemporarilyOpen:!1})}syncTakeoverCount(){this.patch({takeoverCount:this.takeovers.size,isTemporarilyOpen:!1})}syncForceOpenCount(){this.patch({forceOpenCount:this.forceOpeners.size,isTemporarilyOpen:!1})}isForceOpenActive(){return this.state.current.forceOpenCount>0&&"desktop"===this.state.current.viewport}syncLayoutCount(){this.patch({layoutCount:this.layouts.size,isTemporarilyOpen:!1})}patch(e){this.state.set({...this.state.current,...e})}}e.s(["GlobalSidebarManager",0,m,"MAX_EXPANDED_PROJECTS",0,20])},590132,e=>{e.v({chrome:"GlobalSidebarProvider-module__5jJdzq__chrome",desktopSidebarLogo:"GlobalSidebarProvider-module__5jJdzq__desktopSidebarLogo",desktopSidebarOpenIcon:"GlobalSidebarProvider-module__5jJdzq__desktopSidebarOpenIcon",desktopSidebarTrigger:"GlobalSidebarProvider-module__5jJdzq__desktopSidebarTrigger",navMenu:"GlobalSidebarProvider-module__5jJdzq__navMenu",navMenuMobile:"GlobalSidebarProvider-module__5jJdzq__navMenuMobile",navMenuPlaceholder:"GlobalSidebarProvider-module__5jJdzq__navMenuPlaceholder",navMenuResponsiveCollapsed:"GlobalSidebarProvider-module__5jJdzq__navMenuResponsiveCollapsed"})},733065,e=>{"use strict";e.s(["GlobalSidebarNavMenuPlaceholder",()=>Y,"GlobalSidebarProvider",()=>ee,"useCloseGlobalSidebarNavigation",()=>ea,"useGlobalContentAreaObservable",()=>en,"useGlobalSidebar",()=>et,"useGlobalSidebarExpandedConversationIdsObservable",()=>el,"useGlobalSidebarExpandedReplIdsObservable",()=>eo,"useGlobalSidebarLayout",()=>ec,"useGlobalSidebarLayoutActiveObservable",()=>ed,"useGlobalSidebarOrNull",()=>er,"useGlobalSidebarProjectListEnabledObservable",()=>eu,"useGlobalSidebarSettingsPageContent",()=>ei,"useGlobalSidebarTakeover",()=>eg,"useGlobalSidebarViewportObservable",()=>es,"useHideGlobalSidebar",()=>ep,"useIsGlobalSidebarMobileViewport",()=>Z]);var t=e.i(276385),r=e.i(15801),i=e.i(389959);e.i(242933);var a=e.i(790164);e.i(925218);var n=e.i(267103),s=e.i(532764),o=e.i(133522),l=e.i(619297),u=e.i(632350),d=e.i(306229),c=e.i(234504),p=e.i(151027),g=e.i(208018);e.i(214847);var m=e.i(864300),h=e.i(438932),f=e.i(415541),b=e.i(709485),y=e.i(399663),S=e.i(980224),v=e.i(272719),_=e.i(924596),R=e.i(923537),C=e.i(372090),T=e.i(946026),x=e.i(607883),I=e.i(972251),A=e.i(745950),O=e.i(333561),P=e.i(89148),k=e.i(919073),M=e.i(488299),w=e.i(892158),E=e.i(61732),j=e.i(127384),D=e.i(576592),L=e.i(668186),F=e.i(933302),G=e.i(926684),U=e.i(590132);let z={surface:"sidebar"},$=(0,i.createContext)(null),H=new a.ObservableState(null),N=new a.ObservableState("unknown"),V=new a.ObservableState(!1),B=new a.ObservableState([]),W=new a.ObservableState(!1),q=new a.ObservableState(0),K=(0,i.createContext)(null),Q=(0,i.createContext)(null);function Y(){let e=er(),r=e?.navMenuPlaceholderWidth??q,a=(0,i.useRef)(null);return((0,h.useStyleSet)(a,r,e=>({width:e+"px","transition-duration":0===e?"0.2s":"0s"})),e)?(0,t.jsx)(E.View,{innerRef:a,row:!0,align:"center",justify:"center",shrink:0,clsx:U.default.navMenuPlaceholder,"data-global-sidebar-nav-menu-placeholder":"true",style:{width:r.current,height:j.APP_HEADER_HEIGHT,pointerEvents:"none",transitionDuration:0===r.current?"0.2s":"0s"}}):null}function Z(){return"mobile"===(0,n.useObservable)(es())}function X(){return(0,t.jsx)(o.default,{size:16})}function J(e){let r=(0,m.useIntl)(),a=(0,u.default)(),l=(0,n.useObservable)(e.manager.isShellVisible),c=(0,n.useObservable)(e.manager.isOpen),h=(0,n.useObservable)(e.manager.isTemporarilyOpen),_=(0,n.useObservable)(e.manager.viewport),C=(0,n.useObservable)(e.manager.hasCoarsePointer),T="mobile"===_,I=T&&!c,F=null!==e.settingsPageContent,G=T&&c,z=!c&&(T||C||e.hasTouchCapablePointer),$=l&&!("mobile"===_&&c),H=(0,i.useRef)(null),N=(0,i.useRef)(null),V=(0,i.useRef)(!1),B=(0,i.useRef)(!1),W=(0,i.useRef)(!1),q=(0,i.useRef)(!1),K=(0,O.useGlobalSidebarHoverPreview)(e.manager,h),{expandButtonRef:Q}=K,Y=(0,A.useGlobalSidebarChrome)(),{count:Z}=(0,S.default)({skip:!l||!Y.currentUser}),J=(0,R.useFocusCreateComposer)(),ee=(0,y.usePrewarmLibrary)(Y.org?.id,Y.orgResolved);(0,v.useTaskFeedRuntimeBinder)(Y.currentUser?.id??null);let et=(0,i.useCallback)(e=>{N.current=e,z||Q(e)},[Q,z]),er=(0,i.useCallback)(t=>{e.compactSidebarTriggerRef.current=t,z&&Q(t)},[Q,e.compactSidebarTriggerRef,z]);(0,g.default)(()=>{if(!c){q.current&&(e.compactSidebarTriggerRef.current?.focus(),q.current=!1);return}V.current&&(H.current?.focus(),V.current=!1),B.current&&(e.settingsPageContent?.focusDrawerOnOpen(),B.current=!1),W.current&&(N.current?.focus(),W.current=!1)},[c,e.compactSidebarTriggerRef,e.settingsPageContent]);let ei=r.formatMessage({id:"home.globalSidebarNew",defaultMessage:"New"}),ea=r.formatMessage({id:"home.expandSidebar",defaultMessage:"Expand sidebar"}),en=r.formatMessage({id:"home.pinSidebar",defaultMessage:"Pin sidebar"}),es=Y.org?`/t/${Y.org.slug}`:"/home",eo=`${Y.org?es:"/~"}?create=true`,el=r.formatMessage({id:"home.navHomeLabel",defaultMessage:"Home"}),eu=Y.org?es:"/~",ed=l&&(Y.orgResolved||T),ec=T&&!Y.orgResolved,ep=!F&&!ec&&G,eg=!ec&&z,em=!ec&&!ep&&!eg&&!F,eh=em&&a&&!c;return(0,t.jsxs)(k.ShadesSurface,{colorShade:"themeDefault",elevate:!1,background:!1,tag:"div",children:[$?(0,t.jsx)(L.SkipNav,{contentId:"main-content"}):null,(0,t.jsxs)(E.View,{innerRef:K.chromeRef,clsx:U.default.chrome,style:{zIndex:j.HEADER_Z_INDEX+1},onPointerEnter:K.onPointerEnter,onPointerLeave:K.onPointerLeave,onFocusCapture:K.onFocusCapture,onBlurCapture:K.onBlurCapture,onKeyDownCapture:K.onKeyDownCapture,children:[ed?(0,t.jsx)(E.View,{clsx:[U.default.navMenu,T&&U.default.navMenuMobile],"data-global-sidebar-nav-menu":"true",style:{top:T?j.MOBILE_GLOBAL_SIDEBAR_VERTICAL_INSET:void 0,zIndex:j.HEADER_Z_INDEX+1},children:(0,t.jsxs)(k.ShadesSurface,{row:!0,align:"center",justify:"center",shrink:0,background:I,elevate:!!I&&"1x",clsx:[U.default.navMenuPlaceholder,I&&U.default.navMenuResponsiveCollapsed],style:{width:j.COLLAPSED_TASK_SIDEBAR_WIDTH,height:j.APP_HEADER_HEIGHT,pointerEvents:"none"},children:[ec?(0,t.jsx)(E.View,{"aria-hidden":!0,align:"center",justify:"center",width:40,height:40,"data-global-sidebar-toggle-placeholder":"true",children:(0,t.jsx)(X,{})}):null,ep?(0,t.jsx)(w.IconButtonLink,{ref:H,size:40,href:{pathname:es,query:{create:!0}},as:eo,"data-analytics-id":"global_sidebar_logo_create_link","aria-label":ei,alt:ei,onClick:e=>{e.metaKey||e.ctrlKey||e.shiftKey||e.altKey||0!==e.button||((0,d.clearHomeSidebarDestination)(),J()),(0,f.track)(b.events.OPEN_REPL_CREATION_PAGE,{source:"global sidebar",context:(0,p.getOrgTrackingContext)(Y.org??void 0)})},children:(0,t.jsx)(s.default,{size:16,color:P.tokens.brandAccentDefault})}):null,eg?(0,t.jsx)(M.IconButton,{ref:er,size:T?40:28,"data-analytics-id":"global_sidebar_toggle",alt:h?en:ea,"aria-expanded":h,tooltipBehavior:"hidden",onClick:()=>{T?F?B.current=!0:V.current=!0:W.current=!0,e.manager.open()},children:(0,t.jsx)(X,{})}):null,eh?(0,t.jsxs)(M.IconButton,{size:28,clsx:U.default.desktopSidebarTrigger,"data-analytics-id":"global_sidebar_desktop_toggle",alt:h?en:ea,"aria-expanded":h,tooltipBehavior:"hidden",onClick:()=>{W.current=!0,e.manager.open()},children:[(0,t.jsx)(s.default,{clsx:U.default.desktopSidebarLogo,size:24,color:P.tokens.brandAccentDefault}),(0,t.jsx)(o.default,{clsx:U.default.desktopSidebarOpenIcon,size:16})]}):null,em&&!a?(0,t.jsx)(w.IconButtonLink,{size:28,href:es,as:eu,"data-analytics-id":"global_sidebar_logo_home_link","aria-label":el,alt:el,children:(0,t.jsx)(s.default,{size:24,color:P.tokens.brandAccentDefault})}):null]})}):null,(0,t.jsx)(k.ShadesSurface,{background:!1,elevate:"1x",children:(0,t.jsx)(x.GlobalSidebarShell,{manager:e.manager,isVerified:Y.isVerified,org:Y.org,orgResolved:Y.orgResolved,canImport:Y.canImport,canUseIntegrations:Y.canUseIntegrations,canViewRoutines:Y.canViewRoutines,canViewSecurity:Y.canViewSecurity,currentUser:Y.currentUser,notificationCount:Z,onLibraryLinkHover:ee,onInteractionOpenChange:K.onInteractionOpenChange,hideSidebarToggle:z,onSidebarClose:()=>{q.current=T||C||e.hasTouchCapablePointer},sidebarToggleRef:et,settingsPageContent:e.settingsPageContent})})]}),Y.currentUser&&Y.orgResolved?(0,t.jsx)(D.GlobalSearchPaletteHost,{workspace:Y.org}):null]})}function ee(e){let a=(0,i.useRef)();a.current||(a.current=new C.GlobalSidebarManager);let s=a.current,o=(0,i.useRef)(null),[u,d]=(0,i.useState)(!1),p=(0,i.useRef)(new Map),[m,h]=(0,i.useState)(null),f=(0,i.useCallback)(e=>{let t=Symbol("global-sidebar-settings-page-content");return p.current.set(t,e),h(e),()=>{if(!p.current.delete(t))return;let e=Array.from(p.current.values());h(e[e.length-1]??null)}},[]),b=(0,i.useCallback)(()=>{s.handleRouteChange(),window.requestAnimationFrame(()=>{o.current?.focus()})},[s]),y=(0,n.useObservable)(s.isShellVisible),S=(0,n.useObservable)(s.isVisuallyOpen),v=(0,n.useObservable)(s.isRail),x=(0,n.useObservable)(s.isProjectListEnabled),A=y&&(S||v)&&x,O=(0,F.useGetFeatureGate)("gate_sidebar_repl_chateau_status",!1)({disableExposureLog:!0});return(0,g.default)(()=>{let e=window.matchMedia("(pointer: coarse)"),t=window.matchMedia("(hover: none), (any-pointer: coarse)"),r=()=>{s.setHasCoarsePointer(e.matches),d(t.matches),s.setHasTouchCapablePointer(t.matches),s.handleWindowResize(window.innerWidth,window.innerHeight)};s.restoreWidth(),s.restoreOpenState(),r();let i=null,a=()=>{null===i&&(i=window.requestAnimationFrame(()=>{i=null,r()}))};return window.addEventListener("resize",a),e.addListener?e.addListener(r):e.addEventListener("change",r),t.addListener?t.addListener(r):t.addEventListener("change",r),()=>{window.removeEventListener("resize",a),e.removeListener?e.removeListener(r):e.removeEventListener("change",r),t.removeListener?t.removeListener(r):t.removeEventListener("change",r),null!==i&&window.cancelAnimationFrame(i)}},[s]),(0,i.useEffect)(()=>{let e=()=>{s.handleRouteChange()};return r.default.events.on("routeChangeStart",e),()=>{r.default.events.off("routeChangeStart",e)}},[s]),(0,t.jsx)(K.Provider,{value:f,children:(0,t.jsx)(c.AgentStatusSourceProvider,{enabled:A&&!O,replAgentStateEnabled:A,children:(0,t.jsx)($.Provider,{value:s,children:(0,t.jsx)(_.ConversationFeedbackTargetProvider,{children:(0,t.jsxs)(R.CreateComposerFocusProvider,{children:[(0,t.jsx)(G.TrackingHierarchyProvider,{hierarchy:z,children:(0,t.jsx)(l.PreferencesProvider,{children:(0,t.jsx)(J,{compactSidebarTriggerRef:o,manager:s,hasTouchCapablePointer:u,settingsPageContent:m})})}),(0,t.jsx)(T.GlobalSidebarResizeHandle,{manager:s}),(0,t.jsx)(I.GlobalSidebarShortcuts,{manager:s}),(0,t.jsx)(Q.Provider,{value:b,children:e.children})]})})})})})}function et(){let e=(0,i.useContext)($);if(null===e)throw Error("useGlobalSidebar must be used under GlobalSidebarProvider");return e}function er(){return(0,i.useContext)($)}function ei(e){let t=(0,i.useContext)(K);(0,g.default)(()=>t?.(e),[e,t])}function ea(){let e=(0,i.useContext)(Q);if(null===e)throw Error("useCloseGlobalSidebarNavigation must be used under GlobalSidebarProvider");return e}function en(){let e=(0,i.useContext)($);return e?.contentArea??H}function es(){let e=(0,i.useContext)($);return e?.viewport??N}function eo(){let e=(0,i.useContext)($);return e?.expandedReplIds??B}function el(){let e=(0,i.useContext)($);return e?.expandedConversationIds??B}function eu(){let e=(0,i.useContext)($);return e?.isProjectListEnabled??V}function ed(){let e=(0,i.useContext)($);return e?.isLayoutActive??W}function ec(e){let t=(0,i.useContext)($);(0,g.default)(()=>{if(null!==t&&e)return t.addLayout()},[e,t])}function ep(e=!0){let t=(0,i.useContext)($);(0,g.default)(()=>{if(null!==t&&e)return t.addSuppressor()},[t,e])}function eg(e){let t=(0,i.useContext)($);(0,g.default)(()=>{if(null!==t&&e)return t.addTakeover()},[e,t])}},946026,e=>{"use strict";var t=e.i(276385);e.i(925218);var r=e.i(267103),i=e.i(127384),a=e.i(374041);let n=i.SIDEBAR_Z_INDEX+3;e.s(["GlobalSidebarResizeHandle",0,function(e){let s=(0,r.useObservable)(e.manager.isTakenOver);return(0,t.jsx)("div",{style:{position:"fixed",top:0,left:0,width:0,height:0,zIndex:s?i.GLOBAL_SIDEBAR_RESIZE_OVERLAY_Z_INDEX:n},children:(0,t.jsx)(a.SidebarResizeHandle,{rect:e.manager.resizeHandleRect,appearance:"active-only",idleZIndex:1,activeZIndex:2,onResize:{start:()=>e.manager.startResize(),update:t=>e.manager.resize(t.x),end:()=>e.manager.endResize()},onDoubleClick:()=>e.manager.close()})})}])},406718,e=>{e.v({aboveChats:"GlobalSidebarShell-module__82z2PW__aboveChats",aboveChatsCollapsed:"GlobalSidebarShell-module__82z2PW__aboveChatsCollapsed",aboveChatsInner:"GlobalSidebarShell-module__82z2PW__aboveChatsInner",accountCard:"GlobalSidebarShell-module__82z2PW__accountCard",accountCardSettings:"GlobalSidebarShell-module__82z2PW__accountCardSettings","deferred-content-fade-in":"GlobalSidebarShell-module__82z2PW__deferred-content-fade-in","deferred-content-show":"GlobalSidebarShell-module__82z2PW__deferred-content-show",isExpandingFromRail:"GlobalSidebarShell-module__82z2PW__isExpandingFromRail",isHoverPreview:"GlobalSidebarShell-module__82z2PW__isHoverPreview",isOpen:"GlobalSidebarShell-module__82z2PW__isOpen",isRail:"GlobalSidebarShell-module__82z2PW__isRail",isResizing:"GlobalSidebarShell-module__82z2PW__isResizing",list:"GlobalSidebarShell-module__82z2PW__list",railInert:"GlobalSidebarShell-module__82z2PW__railInert",repls:"GlobalSidebarShell-module__82z2PW__repls",root:"GlobalSidebarShell-module__82z2PW__root",scrim:"GlobalSidebarShell-module__82z2PW__scrim",scrimOpen:"GlobalSidebarShell-module__82z2PW__scrimOpen",settingsButton:"GlobalSidebarShell-module__82z2PW__settingsButton",settingsList:"GlobalSidebarShell-module__82z2PW__settingsList",sidebarToggle:"GlobalSidebarShell-module__82z2PW__sidebarToggle",sidebarToggleHidden:"GlobalSidebarShell-module__82z2PW__sidebarToggleHidden",zoomBoardFooterButton:"GlobalSidebarShell-module__82z2PW__zoomBoardFooterButton",zoomRepls:"GlobalSidebarShell-module__82z2PW__zoomRepls",zoomReplsDocked:"GlobalSidebarShell-module__82z2PW__zoomReplsDocked"})},607883,e=>{"use strict";var t=e.i(276385),r=e.i(196786),i=e.i(15801),a=e.i(389959);e.i(925218);var n=e.i(267103),s=e.i(917255),o=e.i(429662),l=e.i(304151),u=e.i(572599),d=e.i(61935),c=e.i(822142),p=e.i(109591),g=e.i(406871),m=e.i(346781),h=e.i(255701),f=e.i(357253),b=e.i(652830),y=e.i(996009),S=e.i(183119),v=e.i(798333),_=e.i(773185),R=e.i(632350),C=e.i(476384),T=e.i(306229),x=e.i(151027),I=e.i(208018),A=e.i(935506);e.i(214847);var O=e.i(864300),P=e.i(2664),k=e.i(438932),M=e.i(415541),w=e.i(709485),E=e.i(174474),j=e.i(327711),D=e.i(3807),L=e.i(270847),F=e.i(777198),G=e.i(448942),U=e.i(289038);e.i(119474);var z=e.i(559357),$=e.i(23818),H=e.i(93837),N=e.i(923537),V=e.i(458495),B=e.i(962915),W=e.i(703415),q=e.i(972251),K=e.i(338806),Q=e.i(535230),Y=e.i(572483),Z=e.i(19715),X=e.i(588005),J=e.i(963587),ee=e.i(23930),et=e.i(288992),er=e.i(89148),ei=e.i(73490),ea=e.i(919073),en=e.i(643484),es=e.i(419635),eo=e.i(488299),el=e.i(892158),eu=e.i(528326),ed=e.i(8047),ec=e.i(244945),ep=e.i(61732),eg=e.i(856919),em=e.i(558407),eh=e.i(127384),ef=e.i(345836),eb=e.i(155606),ey=e.i(933302),eS=e.i(857827),ev=e.i(587719),e_=e.i(197765),eR=e.i(406718);let eC=(0,r.default)(()=>e.A(43862),{loadableGenerated:{modules:[993219]},ssr:!1}),eT=(0,r.default)(()=>e.A(660131).then(e=>e.ChatsListWithConversations),{loadableGenerated:{modules:[781924]},ssr:!1});function ex({footer:e}){let r=(0,O.useIntl)(),i=(0,Z.useSelectedRowClsx)(!0),a=r.formatMessage({id:"globalSidebar.zoomTaskBoard",defaultMessage:"Task Board"}),n=r.formatMessage(e.isBoardOpen?{id:"globalSidebar.chatItemRowCloseTaskBoard",defaultMessage:"Close board for {title}"}:{id:"globalSidebar.chatItemRowOpenTaskBoard",defaultMessage:"Open board for {title}"},{title:e.title});return(0,t.jsx)(ep.View,{grow:!0,br:"default",clsx:i,children:(0,t.jsx)(en.Button,{variant:"nofill",size:"xsmall",stretch:!0,alignment:"center",text:a,textVariant:"text",iconLeft:(0,t.jsx)(y.default,{}),"aria-label":`${a}: ${n}`,"data-analytics-id":"global_sidebar_zoom_board_button","data-analytics-label":"global_sidebar_zoom_board_button","data-analytics-destination":e.analyticsDestination,className:`statsig-no-capture ${eR.default.zoomBoardFooterButton}`,disabled:void 0===e.open,onClick:e.open})})}function eI(e){return(0,t.jsxs)(ea.ShadesSurface,{row:!0,grow:!0,shrink:!0,align:"center",justify:e.compact?"center":void 0,br:12,background:!1,clsx:e.compact?void 0:[eR.default.accountCard,ei.translucentSurfaceClass],children:[(0,t.jsx)(W.GlobalSidebarAccountMenu,{compact:e.compact,currentUser:e.currentUser,environmentsHref:e.environmentsHref,notificationCount:e.notificationCount,onOpenNotifications:e.onOpenNotifications,settingsSurface:e.settingsSurface}),e.compact?null:(0,t.jsx)(ep.View,{clsx:eR.default.accountCardSettings,"data-global-sidebar-deferred":"true",children:(0,t.jsx)(eA,{href:e.settingsHref,as:e.settingsAs,label:e.settingsLabel,orgSlug:e.settingsOrgSlug,settingsPageEnabled:e.settingsPageEnabled,settingsSurface:e.settingsSurface,prefetchSettingsNavigation:e.prefetchSettingsNavigation})})]})}function eA({href:e,as:r,label:i,orgSlug:a,settingsPageEnabled:n,settingsSurface:s,prefetchSettingsNavigation:o}){let l=(0,E.useSettingsLink)("workspaceOverview",a);return(0,ev.usePrefetchUniversalSettingsNavigation)(o?s:{type:"loading"}),(0,t.jsx)(el.IconButtonLink,{size:32,className:eR.default.settingsButton,href:n?e:l.href,as:n?r:l.as,shallow:n?void 0:l.shallow,"data-analytics-id":"global_sidebar_settings_link","aria-label":i,alt:i,onClick:n?void 0:l.onClick,children:(0,t.jsx)(h.default,{})})}function eO(e){let t="string"==typeof e.href?e.href:e.href.pathname,r=t?.split("/").pop()??"unknown";return`global_sidebar_group_nav_${r}_link`}function eP(e){let r=(0,E.useSettingsLink)(e.tab,e.orgSlug);return(0,t.jsx)(ek,{collapsed:e.collapsed,isCurrent:e.isCurrent,href:r.href,as:r.as,shallow:r.shallow,onClick:r.onClick,analyticsId:e.analyticsId,icon:e.icon,text:e.text})}function ek(e){let r=(0,P.useMergeRefs)([e.triggerRef,e.tourTargetRef]),i=(0,Z.useSelectedRowClsx)(e.isCurrent),a=(0,Y.useSidebarNavigationContextMenu)();return e.collapsed?(0,t.jsx)(ep.View,{tag:"li",...a,innerRef:r,br:e.isCurrent?"default":void 0,clsx:i,children:(0,t.jsx)(ec.Tooltip,{tooltip:e.text,placement:"right",children:(r,i)=>(0,t.jsx)(el.IconButtonLink,{...r,ref:i,size:32,variant:"nofill","aria-current":e.isCurrent?"page":void 0,href:e.href,as:e.as,shallow:e.shallow,"data-analytics-id":e.analyticsId,"aria-label":e.text,alt:"",tooltipBehavior:"hidden",onClick:e.onClick,onMouseEnter:t=>{r.onMouseEnter?.(t),e.onMouseEnter?.(t)},children:e.icon})})}):(0,t.jsx)(ep.View,{tag:"li",...a,innerRef:r,br:e.isCurrent?"default":void 0,clsx:i,children:(0,t.jsx)(es.ButtonLink,{...{variant:"ghost","aria-current":e.isCurrent?"page":void 0},alignment:"start",stretch:!0,href:e.href,as:e.as,shallow:e.shallow,"data-analytics-id":e.analyticsId,iconLeft:e.icon,iconRight:e.suffix?(0,t.jsx)(ew,{children:e.suffix}):void 0,onClick:e.onClick,onMouseEnter:e.onMouseEnter,text:e.text})})}function eM(e){let r=(0,O.useIntl)(),i=(0,eb.usePrewarmRepls)({orgId:e.orgId}),n=(0,ey.useFeatureGate)("gate_projects_in_library_tour",!1),o=e.canShowTour&&n&&void 0!==e.userTimeCreated&&Date.parse(e.userTimeCreated)<Date.parse("2026-09-11T00:00:00Z"),l=(0,F.useMemoedDismissibleElement)("projects-now-in-library",void 0,{skip:!o}),[u,d]=(0,a.useState)(null),c=o&&!l.isLoading&&!l.isDone,[p,g]=(0,a.useState)(!1);return(0,Q.useReportSidebarInteractionOpen)(p),(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(ef.RecentReplsHoverCard,{content:e.orgId?(0,t.jsx)(ef.OrgSidebarRecentRepls,{orgId:e.orgId,links:e.hoverLinks}):(0,t.jsx)(ef.SidebarRecentRepls,{links:e.hoverLinks}),isDisabled:e.collapsed||c,onOpenChange:g,children:a=>(0,t.jsx)(ek,{collapsed:e.collapsed,isCurrent:e.isCurrent,href:e.href,analyticsId:"global_sidebar_things_link",icon:(0,t.jsx)(s.default,{}),text:r.formatMessage({id:"home.globalSidebarLibrary",defaultMessage:"Library"}),onMouseEnter:t=>{i(),e.onLibraryLinkHover?.(t)},triggerRef:a,tourTargetRef:d})}),c&&u&&(0,t.jsx)(L.TourPopover,{dataAnalyticsId:"projects_in_library_tour_popover",targetElement:u,doneButtonColorway:!1,colorway:!1,activeStep:{id:"projects-now-in-library",content:r.formatMessage({id:"home.projectsNowInLibrary",defaultMessage:"Projects and Routines are now in your Library"}),hideCloseButton:!0},goto:l.setAsDone,currentStepIndex:0,totalSteps:1,done:l.setAsDone,onClickOutside:l.setAsDone,doneText:r.formatMessage({id:"home.projectsNowInLibraryOk",defaultMessage:"OK"}),showSingleStepDone:!0,disableAnimation:!0,placement:"right"})]})}function ew(e){return(0,t.jsx)(eE,{children:(0,t.jsx)(ep.View,{children:e.children})})}function eE(e){return(0,a.cloneElement)(e.children,{"data-global-sidebar-deferred":"true"})}function ej({item:e,collapsed:r,isCurrent:i}){return(0,t.jsx)(ek,{collapsed:r,isCurrent:i,href:e.href,as:e.as,analyticsId:eO(e),icon:e.icon,text:e.label})}function eD({size:e}){let r=(0,O.useIntl)(),i=(0,em.useOpenGlobalSearchPalette)("global_sidebar"),a=r.formatMessage({id:"home.searchLabel",defaultMessage:"Search"}),n=(0,$.useKeyComboWithFallback)(eg.toggleGlobalSearchPaletteCommand.id,eg.toggleGlobalSearchPaletteCommand.default);return(0,t.jsx)(eo.IconButton,{size:e,variant:"nofill","data-analytics-id":"global_sidebar_search_button","data-global-sidebar-deferred":"true",alt:a,tooltipContents:(0,t.jsxs)(ep.View,{row:!0,gap:8,align:"center",children:[(0,t.jsx)(ed.Text,{multiline:!1,variant:"small",children:a}),n?(0,t.jsx)(z.KeyComboBlocks,{fontSize:10,keyCombo:n}):null]}),tooltipPlacement:J.SIDEBAR_TOOLTIP_PLACEMENT,onClick:()=>i({view:"contextual"}),children:(0,t.jsx)(m.default,{})})}e.s(["GlobalSidebarShell",0,function(e){let r=(0,i.useRouter)(),m=(0,O.useIntl)(),h=(0,R.default)(),y=e.org?{type:"org",orgSlug:e.org.slug}:{type:"personal"},P=(0,e_.useSettingsSurface)({scope:y,disableExposureLog:!0}),E="page"===P.type||"legacyModal"===P.type?P.scope:y,L="page"===P.type,F="org"===E.type?E.orgSlug:null,W=(0,N.useFocusCreateComposer)(),Z=(0,V.useSetCreateComposerOutputKind)(),{createBlankDesignRepl:en}=(0,_.useCreateBlankDesignRepl)({orgId:e.org?.id,trackingData:{forkSource:"design_global_sidebar_new",location:"global_sidebar"}}),{createEmptyRepl:es}=(0,v.useCreateEmptyRepl)({orgId:e.org?.id,trackingData:{forkSource:"empty_global_sidebar_new",location:"global_sidebar"}}),el=(0,S.useFabricOnlyAppCreation)(e.org?.id),ec=!el.isLocked&&!el.isResolving,eg=(0,ey.useFeatureGate)("gate_public_template_marketplace",!1),em=(0,n.useObservable)(e.manager.isShellVisible),ef=(0,n.useObservable)(e.manager.isOpen),eb=(0,n.useObservable)(e.manager.isTemporarilyOpen),ev=(0,n.useObservable)(e.manager.isVisuallyOpen),eA=(0,n.useObservable)(e.manager.viewport),ew=(0,n.useObservable)(e.manager.isResizing),eL=(0,n.useObservable)(e.manager.isTakenOver),eF=(0,n.useObservable)(e.manager.isCollapsedToRail),eG=(0,n.useObservable)((0,ee.zoomBoardFooterState)()),eU=(0,n.useObservable)((0,et.zoomChromeCollapseState)()),ez=(0,n.useObservable)((0,et.zoomChromeCollapseEligibleState)()),e$=(0,a.useRef)(eF),[eH,eN]=(0,a.useState)(!1),eV=(0,A.usePrefersReducedMotion)(),eB="mobile"===eA?eh.APP_HEADER_HEIGHT+2*eh.MOBILE_GLOBAL_SIDEBAR_VERTICAL_INSET:eh.APP_HEADER_HEIGHT,eW=(0,eS.settingsQueryFromPath)(r.asPath),eq=L?(0,eS.genericSettingsLocation)(E,"workspaceOverview",eW):(0,eS.legacySettingsLocation)(E,"workspaceOverview",eW),eK=m.formatMessage({id:"home.navSettingsLabel",defaultMessage:"Settings"}),eQ=m.formatMessage({id:"home.toggleSidebar",defaultMessage:"Toggle sidebar"}),eY=m.formatMessage({id:"home.pinSidebar",defaultMessage:"Pin sidebar"}),eZ=(0,$.useKeyComboWithFallback)(q.toggleGlobalSidebarCommand.id,q.defaultToggleGlobalSidebarShortcut),eX=eq.href,eJ="aliased"===eq.type?eq.as:void 0,e0=e.notificationCount??0,e1=(0,x.useOptionalStoredOrgContext)(),e2=e.orgResolved&&(void 0===e1||e1.orgId===e.org?.id&&(e1.isAuthoritative||e1.resolutionFailed||e1.isOrgContextSkipped&&!e1.loading)),e5=(0,ey.useScopedFeatureGate)("gate_nexus_environments_v1",{key:e.org?.id??"personal",ready:e2}),e3=(0,ey.useScopedFeatureGate)("gate_routines_ux",{key:e.org?.id??"personal",ready:e.isVerified&&e2}),e8=e.isVerified&&e.canViewRoutines,e7=!eV&&!ew&&!eF&&e$.current&&"desktop"===eA&&ev,e9=e7||eH,[e4,e6]=(0,a.useState)(!1),te=(0,a.useRef)(!1);(0,I.default)(()=>{let e=te.current;if(te.current="desktop"===eA&&!eF&&ev,!eF||!e||ew||"desktop"!==eA||eV)return void e6(!1);e6(!0);let t=window.setTimeout(()=>e6(!1),200);return()=>window.clearTimeout(t)},[eF,ew,ev,eV,eA]);let tt=eF&&!e4,tr="string"==typeof r.query.orgSlug?r.query.orgSlug:e.org?.slug,ti=e.org?(0,G.orgLinks)({slug:e.org.slug}):void 0,ta=ti?.repls??{href:"/repls",routerPath:"/repls"},tn=ti?.library??{href:"/library",routerPath:"/library"},ts=ti?.connectors??{href:"/~/integrations",routerPath:"/integrations"},to=ti?.environments??{href:"/environments",routerPath:"/environments"},tl=ti?.security??{href:"/security",routerPath:"/security"},tu=L?(0,eS.genericSettingsLocation)(E,"templates",eW):(0,eS.legacySettingsLocation)(E,"templates",eW),td=L?(0,eS.dedicatedSettingsLocation)(E,"integrations",eW):(0,eS.legacySettingsLocation)(E,"integrations",eW),tc=L?(0,eS.dedicatedSettingsLocation)(E,"security",eW):(0,eS.legacySettingsLocation)(E,"security",eW),tp=e.org?{href:`/t/${e.org.slug}/routines`,routerPath:"/t/[orgSlug]/routines"}:{href:"/routines",routerPath:"/routines"},tg=e.org?`/t/${e.org.slug}`:"/home",tm=e.org?tg:"/~",th=ti?.home.routerPath??"/home",tf="true"===r.query[j.SETTINGS_SHOW_PARAM]?r.query[D.SETTINGS_TAB_PARAM]:void 0,tb=L&&("/settings/[tab]"===r.pathname||"/t/[orgSlug]/settings/[tab]"===r.pathname),ty=tb&&null!==e.settingsPageContent,tS=ty&&e.settingsPageContent?.navigationDisabled===!0,tv=e.orgResolved&&e.isVerified&&!ty,t_=tv&&em&&(ev||eF),tR=tb?"templates"===r.query.tab:eg&&"templates"===tf,tC=tb||tR||!L&&"integrations"===tf||!L&&"security"===tf,tT=e=>!tC&&r.pathname===e,tx=tT(th)||h&&tT("/desktopApp/home2"),tI=null;if(eg){let r=m.formatMessage({id:"home.navTemplatesLabel",defaultMessage:"Templates"});tI=L?(0,t.jsx)(ek,{collapsed:tt,href:tu.href,as:"aliased"===tu.type?tu.as:void 0,isCurrent:tR,analyticsId:"global_sidebar_templates_link",icon:(0,t.jsx)(c.default,{}),text:r}):(0,t.jsx)(eP,{tab:"templates",collapsed:tt,orgSlug:e.org?.slug??null,isCurrent:tR,analyticsId:"global_sidebar_templates_link",icon:(0,t.jsx)(c.default,{}),text:r})}let tA=null;if(e.canUseIntegrations){let r=m.formatMessage({id:"home.navIntegrationsLabel",defaultMessage:"Integrations"});tA=L?(0,t.jsx)(ek,{collapsed:tt,href:td.href,as:"aliased"===td.type?td.as:void 0,isCurrent:tT(ts.routerPath),analyticsId:"global_sidebar_integrations_link",icon:(0,t.jsx)(d.default,{}),text:r}):(0,t.jsx)(eP,{tab:"integrations",collapsed:tt,orgSlug:e.org?.slug??null,isCurrent:"integrations"===tf,analyticsId:"global_sidebar_integrations_link",icon:(0,t.jsx)(d.default,{}),text:r})}let tO=null;if(e.canViewSecurity){let r=m.formatMessage({id:"home.navSecurityLabel",defaultMessage:"Security"});tO=L?(0,t.jsx)(ek,{collapsed:tt,href:tc.href,as:"aliased"===tc.type?tc.as:void 0,isCurrent:tT(tl.routerPath),analyticsId:"global_sidebar_security_link",icon:(0,t.jsx)(f.default,{}),text:r}):(0,t.jsx)(eP,{tab:"security",collapsed:tt,orgSlug:e.org?.slug??null,isCurrent:"security"===tf,analyticsId:"global_sidebar_security_link",icon:(0,t.jsx)(f.default,{}),text:r})}let tP=()=>{(0,M.track)(w.events.OPEN_REPL_CREATION_PAGE,{source:"global sidebar",context:(0,x.getOrgTrackingContext)(e.org??void 0)})},tk=()=>{(0,M.track)(w.events.OPEN_IMPORT_PAGE,{source:"global sidebar",href:`${window.location.origin}${window.location.pathname}`,context:(0,x.getOrgTrackingContext)(e.org??void 0)})},tM=(0,U.useOrgGroupNavItems)({orgSlug:tr}),tw=r.pathname.startsWith(G.groupBaseRouterPath)&&tM.length>0,tE=t_&&!tw,tj=ez&&null!==eU&&"desktop"===eA&&tE&&!eF&&!eb&&!eL,tD=tj&&"project"===eU,tL="exited"!==(0,H.useHomeBlankSlatePhase)()&&"desktop"===eA&&!eb,tF=(0,a.useRef)(null);(0,k.useStyleSet)(tF,e.manager.shellWidth,e=>({width:e+"px"}),[em,e.orgResolved]),(0,I.default)(()=>{tF.current&&(tF.current.inert=!ev&&!eF)},[ev,eF,em]),(0,I.default)(()=>{if(e7){eN(!0);let e=window.setTimeout(()=>{e$.current=!1,eN(!1)},300);return()=>window.clearTimeout(e)}(eF||ew||!ev||"desktop"!==eA||eV)&&eN(!1),e$.current=eF},[eF,e7,ev,eV,eA]),(0,I.default)(()=>(e.manager.setProjectListEnabled(tv),()=>e.manager.setProjectListEnabled(!1)),[tv,e.manager]);let[tG,tU]=(0,a.useState)(!1),tz=e.onInteractionOpenChange,t$=(0,a.useCallback)(e=>{tU(e)},[]),[tH,tN]=(0,a.useState)(!1);(0,I.default)(()=>{em||tN(!1)},[em]),(0,I.default)(()=>{tz?.(tG||tH)},[tG,tH,tz]);let tV=eb?eY:eQ,tB=(0,t.jsx)(eo.IconButton,{ref:e.sidebarToggleRef,alt:tV,tooltipPlacement:J.SIDEBAR_TOOLTIP_PLACEMENT,tooltipContents:(0,t.jsxs)(ep.View,{row:!0,gap:8,align:"center",children:[(0,t.jsx)(ed.Text,{multiline:!1,variant:"small",children:tV}),eZ?(0,t.jsx)(z.KeyComboBlocks,{fontSize:10,keyCombo:eZ}):null]}),size:28,"data-analytics-id":eb?"global_sidebar_pin_button":"global_sidebar_toggle","data-global-sidebar-deferred":e.hideSidebarToggle?void 0:"true","aria-expanded":ev,tabIndex:e.hideSidebarToggle?-1:void 0,clsx:[eR.default.sidebarToggle,e.hideSidebarToggle&&eR.default.sidebarToggleHidden],onClick:()=>{eb?e.manager.open():(ef&&e.onSidebarClose?.(),e.manager.toggle())},children:(0,t.jsx)(b.default,{})});if(!em||!e.orgResolved)return null;let tW=(0,t.jsx)(B.CreateOutputKindHoverCard,{isDisabled:tt,projectActions:{onSelectChat:()=>{tP(),(0,T.clearHomeSidebarDestination)(),W(),r.push({pathname:tg,query:{create:!0}},tm)},onImport:e.canImport?()=>{(0,T.clearHomeSidebarDestination)(),tk()}:void 0,onCreateBlankDesign:ec?()=>{(0,T.clearHomeSidebarDestination)(),en()}:void 0,onCreateEmptyProject:ec?()=>{(0,T.clearHomeSidebarDestination)(),es()}:void 0},onSelect:e=>{Z(e),tP(),(0,T.clearHomeSidebarDestination)(),W(),r.push({pathname:tg,query:{create:!0}},tm)},children:e=>(0,t.jsx)(ek,{triggerRef:e,collapsed:tt,isCurrent:tx,href:{pathname:tg,query:{create:!0}},as:tm,analyticsId:"global_sidebar_create_link",icon:(0,t.jsx)(g.default,{}),onClick:e=>{e.metaKey||e.ctrlKey||e.shiftKey||e.altKey||0!==e.button||((0,T.clearHomeSidebarDestination)(),W()),tP()},text:m.formatMessage({id:"home.globalSidebarNew",defaultMessage:"New"})})}),tq=[],tK=[{label:m.formatMessage({id:"home.globalSidebarProjects",defaultMessage:"Projects"}),icon:(0,t.jsx)(p.default,{}),href:ta.href}];e8&&tK.push({label:m.formatMessage({id:"home.globalSidebarRoutines",defaultMessage:"Routines"}),icon:(0,t.jsx)(o.default,{}),href:tp.href}),h||tK.push({label:m.formatMessage({id:"home.libraryAssetsTab",defaultMessage:"Assets"}),icon:(0,t.jsx)(u.default,{}),href:tn.href}),e.canImport&&tq.push({id:"import",label:m.formatMessage({id:"home.globalSidebarImport",defaultMessage:"Import"}),icon:(0,t.jsx)(l.default,{}),element:(0,t.jsx)(ek,{collapsed:tt,isCurrent:tT("/import")||tT("/import/[provider]"),href:"/import",analyticsId:"global_sidebar_import_link",icon:(0,t.jsx)(l.default,{}),onClick:tk,text:m.formatMessage({id:"home.globalSidebarImport",defaultMessage:"Import"})})}),tq.push({id:"things",label:m.formatMessage({id:"home.globalSidebarLibrary",defaultMessage:"Library"}),icon:(0,t.jsx)(s.default,{}),element:(0,t.jsx)(eM,{collapsed:tt,isCurrent:tT(ta.routerPath)||tT(tp.routerPath)||tT(tn.routerPath),href:ta.href,orgId:e.org?.id,hoverLinks:tK,onLibraryLinkHover:e.onLibraryLinkHover,userTimeCreated:e.currentUser?.timeCreated,canShowTour:"desktop"===eA&&em&&ef&&!eL&&!tj})}),null!==tI&&tq.push({id:"templates",label:m.formatMessage({id:"home.navTemplatesLabel",defaultMessage:"Templates"}),icon:(0,t.jsx)(c.default,{}),element:tI}),null!==tA&&tq.push({id:"integrations",label:m.formatMessage({id:"home.navIntegrationsLabel",defaultMessage:"Integrations"}),icon:(0,t.jsx)(d.default,{}),element:tA}),null!==tO&&tq.push({id:"security",label:m.formatMessage({id:"home.navSecurityLabel",defaultMessage:"Security"}),icon:(0,t.jsx)(f.default,{}),element:tO});let tQ=tw?tM.map(e=>({id:eO(e),label:e.label,icon:e.icon,element:(0,t.jsx)(ej,{item:e,collapsed:tt,isCurrent:e.active?.(r)??!1})})):tq,tY=(0,t.jsx)(Y.SidebarNavigationItems,{items:tQ,collapsed:tt}),tZ=(0,t.jsxs)(t.Fragment,{children:[tw?null:tW,tY]}),tX=tE?(0,t.jsx)(ep.View,{tag:"li",grow:!0,shrink:!0,clsx:[eR.default.repls,ez&&eR.default.zoomRepls,tD&&eR.default.zoomReplsDocked],children:(0,t.jsx)(eT,{collapsed:tt,orgId:e.org?.id,orgSlug:e.org?.slug,routinesEnabled:e8&&e3})}):null;return(0,t.jsxs)(Q.SidebarInteractionOpenProvider,{onOpenChange:t$,children:[(0,t.jsx)(ea.ShadesSurface,{tag:"nav",innerRef:tF,colorShade:"themeDefault",elevate:!h&&"1x",background:!tL,"aria-label":m.formatMessage({id:"home.globalSidebarNavLabel",defaultMessage:"Main navigation"}),"data-analytics-id":"global_sidebar","data-global-sidebar-root":"true","data-global-sidebar-viewport":eA,"data-global-sidebar-open":ef?"true":void 0,"data-global-sidebar-rail":eF?"true":void 0,"data-global-sidebar-collapsing":e4?"true":void 0,"data-global-sidebar-expanding":e9?"true":void 0,"data-global-sidebar-interaction-open":tG||tH?"true":void 0,clsx:[eR.default.root,{[eR.default.isOpen]:ev,[eR.default.isRail]:eF,[eR.default.isResizing]:ew,[eR.default.isHoverPreview]:eb,[eR.default.isExpandingFromRail]:e9,[ei.translucentSurfaceClass]:tL}],style:{zIndex:eh.SIDEBAR_Z_INDEX,boxShadow:tL?er.tokens.shadowSubtleOverlayBorder:void 0,...eL?{height:eh.APP_HEADER_HEIGHT,overflow:"hidden"}:null},children:(0,t.jsxs)(Y.SidebarNavigationCustomization,{storageKey:`global-sidebar-hidden-nav:${e.currentUser?.id??"anonymous"}:${e.org?.id??"personal"}`,items:tQ,children:[(0,t.jsxs)(ea.ShadesSurface,{row:!0,align:"center",justify:"end",shrink:0,px:6,background:!1,elevate:!1,style:{height:eB},children:[ty?(0,t.jsx)(ep.View,{grow:!0,"data-analytics-product-area":"settings",children:e.settingsPageContent?.header}):null,ty||tt?null:(0,t.jsx)(eD,{size:28}),!ty&&e.hideSidebarToggle?(0,t.jsx)(ep.View,{"aria-hidden":!0,children:tB}):null,ty||e.hideSidebarToggle?null:tB]}),e.currentUser?(0,t.jsx)(ep.View,{px:6,pt:4,pb:4,shrink:0,"aria-hidden":eL||tS,inert:eL||tS?"":void 0,"data-tour-id":eF?void 0:"home-arrival-workspace-switcher",clsx:eR.default.railInert,children:(0,t.jsx)(K.GlobalSidebarWorkspaceSwitcher,{compact:tt,org:e.org,currentUser:e.currentUser,onWorkspaceSwitch:ty?e.settingsPageContent?.onWorkspaceSwitch:void 0})}):null,ty?(0,t.jsx)(ep.View,{tag:"ul",px:6,pt:4,grow:!0,shrink:!0,"aria-hidden":eL,inert:eL?"":void 0,clsx:[eR.default.list,eR.default.railInert,eR.default.settingsList],children:(0,t.jsx)(ep.View,{tag:"li","data-analytics-product-area":"settings","data-settings-sidebar-navigation":"true",children:e.settingsPageContent?.navigation})}):null,!ty&&ez?(0,t.jsxs)(t.Fragment,{children:[tw?null:(0,t.jsx)(ep.View,{shrink:0,"aria-hidden":tD||void 0,inert:tD?"":void 0,clsx:[eR.default.aboveChats,tD&&eR.default.aboveChatsCollapsed],children:(0,t.jsx)(ep.View,{justify:"end",clsx:eR.default.aboveChatsInner,children:(0,t.jsx)(ep.View,{tag:"ul",px:6,pt:4,gap:4,shrink:0,clsx:eR.default.list,children:tW})})}),(0,t.jsx)(ep.View,{shrink:!tE||0,grow:!tE||void 0,"aria-hidden":tj||void 0,inert:tj?"":void 0,"data-global-sidebar-zoom-chrome-collapsed":tj?"true":void 0,clsx:[eR.default.aboveChats,tj&&eR.default.aboveChatsCollapsed],children:(0,t.jsx)(ep.View,{justify:"end",clsx:eR.default.aboveChatsInner,children:(0,t.jsx)(ep.View,{tag:"ul",px:6,pt:4,gap:4,grow:!tE||void 0,shrink:!tE||0,"aria-hidden":eL,inert:eL?"":void 0,clsx:[eR.default.list,eR.default.railInert],children:tY})})}),tE?(0,t.jsx)(ep.View,{tag:"ul",px:6,pt:4,grow:!0,shrink:!0,"aria-hidden":eL,inert:eL?"":void 0,clsx:[eR.default.list,eR.default.railInert],children:tX}):null]}):null,ty||ez?null:(0,t.jsxs)(ep.View,{tag:"ul",px:6,pt:4,gap:4,grow:!0,shrink:!0,"aria-hidden":eL,inert:eL?"":void 0,clsx:[eR.default.list,eR.default.railInert],children:[tZ,tX]}),!ev||eF||e9||eL||tS||!e.orgResolved||null!==e.org?null:(0,t.jsx)(eE,{children:(0,t.jsx)(ep.View,{children:(0,t.jsx)(X.StarterPlanUsageCard,{})})}),!e.currentUser||eL||eF||tS?null:(0,t.jsx)(C.HomeArrivalTourEntryPoint,{presentation:"sidebar"}),e.currentUser?(0,t.jsx)(ea.ShadesSurface,{row:!0,align:"center",gap:4,px:6,py:8,shrink:0,"aria-hidden":eL||tS,inert:eL||tS?"":void 0,background:!1,elevate:!1,clsx:eR.default.railInert,children:eF||null===eG?(0,t.jsx)(eI,{compact:tt,currentUser:e.currentUser,environmentsHref:e5&&!h?to.href:void 0,settingsHref:eX,settingsAs:eJ,settingsLabel:eK,settingsOrgSlug:F,settingsPageEnabled:L,settingsSurface:P,prefetchSettingsNavigation:!tb,notificationCount:e0,onOpenNotifications:()=>tN(!0)}):(0,t.jsx)(ex,{footer:eG})}):null]},`${e.currentUser?.id}:${e.org?.id??"personal"}`)}),(0,t.jsx)(eu.Modal,{isOpen:tH,onRequestClose:()=>tN(!1),dataAnalyticsId:"global_sidebar_notifications_modal",children:(0,t.jsxs)(ep.View,{gap:12,children:[(0,t.jsx)(ed.Text,{variant:"subheadDefault",children:m.formatMessage({id:"home.notifications",defaultMessage:"Notifications"})}),(0,t.jsx)(eC,{count:10,loadMore:!0,markAsSeen:!0})]})}),"mobile"===eA?(0,t.jsx)("button",{type:"button","data-analytics-id":"global_sidebar_scrim_overlay","aria-label":m.formatMessage({id:"home.globalSidebarCloseOverlay",defaultMessage:"Close sidebar"}),"aria-hidden":!ef,tabIndex:ef?0:-1,onClick:()=>{e.onSidebarClose?.(),e.manager.close()},clsx:[eR.default.scrim,{[eR.default.scrimOpen]:ef}],style:{zIndex:eh.SIDEBAR_OVERLAY_Z_INDEX}}):null]})}])},972251,e=>{"use strict";var t=e.i(276385),r=e.i(119474),i=e.i(23818);let a=(0,e.i(295621).defaultKeyCombo)({cmdOrCtrl:!0,shift:!0,key:"e"}),n={id:"globalSidebar.toggle",name:"Toggle navigation sidebar",nameId:"globalSidebar.toggleCommandName",description:"Opens or closes the navigation sidebar",descriptionId:"globalSidebar.toggleCommandDescription",default:a},s=[n];function o(e){let{manager:t}=e;return(0,i.useCommandBinding)(()=>[n.id,{run:()=>{if(!t.isShellVisible.current)return!1;t.toggle()},bubble:!1}],[t]),null}e.s(["GlobalSidebarShortcuts",0,function(e){return(0,t.jsx)(r.KeybindingsSurface,{commands:s,children:(0,t.jsx)(o,{manager:e.manager})})},"defaultToggleGlobalSidebarShortcut",0,a,"toggleGlobalSidebarCommand",0,n])},398289,e=>{"use strict";var t=e.i(351623),r=e.i(884033),i=e.i(344480);e.i(975473);let a={},n=t.gql`
    query GetGlobalSidebarWorkspaceSwitcher {
  currentUser {
    ...WorkspaceDropdownCurrentUser
  }
}
    ${r.WorkspaceDropdownCurrentUserFragmentDoc}`;e.s(["useGetGlobalSidebarWorkspaceSwitcherQuery",0,function(e){let t={...a,...e};return i.useQuery(n,t)}])},338806,e=>{"use strict";var t=e.i(276385),r=e.i(196786),i=e.i(389959),a=e.i(398289),n=e.i(294827);e.i(214847);var s=e.i(864300),o=e.i(573380),l=e.i(486131),u=e.i(535230),d=e.i(773222);let c=(0,r.default)(()=>e.A(798227).then(e=>e.GlobalSidebarWorkspaceSwitcherMenu),{ssr:!1,loading:({error:e})=>(0,t.jsx)(o.GlobalSidebarPopoverBodyState,{error:e})});e.s(["GlobalSidebarWorkspaceSwitcher",0,function({org:r,currentUser:o,compact:p=!1,onWorkspaceSwitch:g}){let m=(0,s.useIntl)(),[h,f]=(0,i.useState)(!1),[b,y]=(0,i.useState)(!1);(0,u.useReportSidebarInteractionOpen)(h);let{shouldHidePersonalWorkspace:S}=(0,n.usePersonalWorkspacesDisabled)(),v=(0,i.useRef)(null),{data:_}=(0,a.useGetGlobalSidebarWorkspaceSwitcherQuery)({fetchPolicy:"cache-and-network",ssr:!1,skip:!b});(0,i.useEffect)(()=>{e.A(798227).catch(()=>{})},[]);let R=m.formatMessage({id:"home.globalSidebarWorkspaceMenu",defaultMessage:"Workspace menu"});return(0,t.jsxs)(d.PopoverTrigger,{isOpen:h,onOpenChange:e=>{f(e),e&&y(!0)},placement:"bottom start",label:R,scrollRef:v,children:[e=>(0,t.jsx)(l.GlobalSidebarWorkspaceSwitcherTrigger,{org:r,currentUser:o,compact:p,isOpen:h,label:R,triggerProps:e}),(0,t.jsx)(c,{currentUser:_?.currentUser??null,currentOrgId:r?.id,scrollRef:v,onRoute:()=>f(!1),shouldHidePersonalWorkspace:S,onWorkspaceSwitch:g})]})}])},701164,e=>{e.v({compactTrigger:"GlobalSidebarWorkspaceSwitcherTrigger-module__kuolnW__compactTrigger",trigger:"GlobalSidebarWorkspaceSwitcherTrigger-module__kuolnW__trigger"})},486131,e=>{"use strict";var t=e.i(276385),r=e.i(167392),i=e.i(568430);e.i(214847);var a=e.i(864300),n=e.i(25561),s=e.i(276887),o=e.i(825419),l=e.i(643484),u=e.i(488299),d=e.i(61732),c=e.i(701164);e.s(["GlobalSidebarWorkspaceSwitcherTrigger",0,function({org:e,currentUser:p,compact:g=!1,isOpen:m,label:h,triggerProps:f}){let b=(0,a.useIntl)(),y=e?.name??p.firstName??p.username,S=e?e.image:p.image,v=e?(0,s.getFormattedOrgWorkspaceName)({ownerName:y,isInOrg:!0}):b.formatMessage({id:"home.personalWorkspaceSwitcher",defaultMessage:"Personal workspace"}),_=e&&!S?(0,t.jsx)(n.DefaultOrgIcon,{}):(0,t.jsx)(o.Avatar,{src:S,username:y,size:24}),{ref:R,...C}=f;return(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(u.IconButton,{...C,ref:g?R:void 0,style:g?void 0:{display:"none"},alt:h,size:32,clsx:c.default.compactTrigger,tooltipPlacement:"right","data-analytics-id":g?"global_sidebar_workspace_menu_button":void 0,children:_}),(0,t.jsx)(l.Button,{...C,ref:g?void 0:R,style:g?{display:"none"}:void 0,variant:"filledAndOutlined",colorway:"control",alignment:"start",stretch:!0,borderRadius:"full",className:c.default.trigger,iconLeft:_,iconRight:(0,t.jsx)(d.View,{"data-global-sidebar-deferred":"true",children:m?(0,t.jsx)(i.default,{}):(0,t.jsx)(r.default,{})}),text:v,"data-analytics-id":g?void 0:"global_sidebar_workspace_menu_button","data-analytics-label":"workspace_menu"})]})}])},632530,e=>{e.v({customize:"SidebarNavigationCustomization-module__fRQXba__customize",more:"SidebarNavigationCustomization-module__fRQXba__more",moreDivider:"SidebarNavigationCustomization-module__fRQXba__moreDivider",moreList:"SidebarNavigationCustomization-module__fRQXba__moreList",option:"SidebarNavigationCustomization-module__fRQXba__option"})},572483,e=>{"use strict";var t=e.i(276385),r=e.i(15801),i=e.i(389959),a=e.i(30538),n=e.i(443893),s=e.i(927225),o=e.i(255701);e.i(214847);var l=e.i(864300),u=e.i(535230),d=e.i(257418),c=e.i(769385),p=e.i(406664),g=e.i(643484),m=e.i(86145),h=e.i(903790),f=e.i(965531),b=e.i(488299),y=e.i(295231),S=e.i(773222),v=e.i(8047),_=e.i(61732),R=e.i(933302),C=e.i(632530);let T=(0,i.createContext)(null);function x(){let e=(0,i.useContext)(T);return{onContextMenu:e?.onContextMenu}}function I({items:e,storageKey:r,children:a}){let n=(0,l.useIntl)(),{hiddenIds:s,save:p}=(0,c.useSidebarNavigationPreferences)(r),[g,m]=(0,i.useState)({type:"closed"});(0,u.useReportSidebarInteractionOpen)("closed"!==g.type);let h=n.formatMessage({id:"home.sidebarNavCustomize",defaultMessage:"Customize"}),f=(0,i.useCallback)(e=>{e.preventDefault();let t=0===e.clientX&&0===e.clientY,r=e.currentTarget.getBoundingClientRect();m({type:"contextMenu",position:t?{x:r.left,y:r.bottom}:{x:e.clientX,y:e.clientY},anchor:e.currentTarget})},[]),b=(0,i.useMemo)(()=>({hiddenIds:s,save:p,onContextMenu:f}),[s,p,f]),v=()=>{"customize"===g.type&&(p(g.hiddenIds),m({type:"closed"}))};return(0,t.jsxs)(T.Provider,{value:b,children:[a,"contextMenu"===g.type?(0,t.jsx)(d.SidebarRowContextMenu,{position:g.position,isOpen:!0,triggerLabel:h,onOpenChange:e=>{e||m(e=>"contextMenu"===e.type?{type:"closed"}:e)},children:(0,t.jsx)(y.MenuItem,{icon:(0,t.jsx)(o.default,{}),label:h,"data-analytics-id":"global_sidebar_customize_item",onAction:()=>m({type:"customize",anchor:g.anchor,hiddenIds:s})})}):null,"customize"===g.type?(0,t.jsx)(S.RawPopover,{isOpen:!0,triggerRef:{current:g.anchor},placement:"right top",offset:4,onOpenChange:e=>{e||v()},"data-analytics-id":"global_sidebar_customize_popover",children:(0,t.jsx)(A,{items:e,hiddenIds:g.hiddenIds,onChange:e=>m(t=>"customize"===t.type?{...t,hiddenIds:e}:t),onDone:v})}):null]})}function A({items:e,hiddenIds:r,onChange:a,onDone:s}){let o=(0,l.useIntl)(),u=(0,i.useRef)(null),d=o.formatMessage({id:"home.sidebarNavCustomizeTitle",defaultMessage:"Choose what appears in your sidebar"});return(0,i.useEffect)(()=>{u.current?.focus()},[]),(0,t.jsx)(n.Dialog,{"aria-label":d,children:(0,t.jsxs)(_.View,{p:8,gap:4,clsx:C.default.customize,children:[(0,t.jsx)(_.View,{px:8,py:8,children:(0,t.jsx)(v.Text,{children:d})}),e.map((e,i)=>(0,t.jsxs)(p.Interactive,{tag:"label",variant:"nofill",row:!0,align:"center",gap:8,px:8,py:4,br:8,clsx:C.default.option,"data-analytics-id":"global_sidebar_nav_visibility_checkbox",children:[(0,t.jsx)(m.Checkbox,{ref:0===i?u:void 0,ariaLabel:e.label,checked:!r.includes(e.id),onChange:t=>a(t?r.filter(t=>t!==e.id):[...r,e.id])}),e.icon,(0,t.jsx)(v.Text,{children:e.label})]},e.id)),(0,t.jsx)(_.View,{px:4,pt:8,pb:4,children:(0,t.jsx)(g.Button,{stretch:!0,text:o.formatMessage({id:"home.sidebarNavCustomizeDone",defaultMessage:"Done"}),onClick:s,"data-analytics-id":"global_sidebar_customize_done_button"})})]})})}function O({items:e,hiddenItems:n,hiddenIds:d,save:c,collapsed:p}){let m=(0,l.useIntl)(),y=x(),[S,v]=(0,i.useState)({type:"closed"}),R="closed"!==S.type,I=(0,i.useRef)(null),P=(0,i.useRef)(null),k=(0,i.useRef)(null),M=(0,i.useRef)(!1),{events:w}=(0,r.useRouter)();(0,i.useEffect)(()=>{let e=()=>k.current?.();return w.on("routeChangeStart",e),()=>w.off("routeChangeStart",e)},[w]);let E=(0,i.useCallback)(e=>{!e&&M.current&&(M.current=!1,P.current?.focus(),k.current?.()),e||"customize"!==S.type||c(S.hiddenIds),v(t=>e&&"closed"===t.type?{type:"menu"}:e||"closed"===t.type?t:{type:"closed"})},[S,c]),j=()=>{"customize"===S.type&&c(S.hiddenIds),v({type:"closed"}),k.current?.()};(0,a.useInteractOutside)({ref:I,isDisabled:"customize"!==S.type,onInteractOutside:j}),(0,u.useReportSidebarInteractionOpen)(R);let D=m.formatMessage({id:"home.sidebarNavMore",defaultMessage:"More"});return(0,t.jsx)(f.HoverCard,{closeRef:k,opensOnClick:!0,openDelayMs:500,placement:"right top",offset:4,contentPadding:8*("customize"!==S.type),onOpenChange:E,content:(0,t.jsx)(T.Provider,{value:null,children:(0,t.jsx)(_.View,{innerRef:I,gap:4,clsx:C.default.more,"data-analytics-id":"global_sidebar_more_popover",onKeyDown:e=>{"Escape"===e.key&&(e.preventDefault(),M.current=!0,j())},children:"customize"===S.type?(0,t.jsx)(A,{items:e,hiddenIds:S.hiddenIds,onChange:e=>v(t=>"customize"===t.type?{...t,hiddenIds:e}:t),onDone:j}):(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(_.View,{tag:"ul",gap:4,clsx:C.default.moreList,"aria-label":D,children:n.map(e=>(0,i.cloneElement)(e.element,{key:e.id,collapsed:!1}))}),(0,t.jsx)(_.View,{role:"separator",py:4,children:(0,t.jsx)(h.DividerH,{className:C.default.moreDivider})}),(0,t.jsx)(g.Button,{variant:"ghost",alignment:"start",stretch:!0,text:m.formatMessage({id:"home.sidebarNavCustomize",defaultMessage:"Customize"}),iconLeft:(0,t.jsx)(o.default,{}),onClick:()=>v({type:"customize",hiddenIds:d}),"data-analytics-id":"global_sidebar_customize_item"})]})})}),children:e=>(0,t.jsx)(_.View,{tag:"li",innerRef:e,...y,onKeyDown:e=>{if(R&&"Escape"===e.key){e.preventDefault(),M.current=!0,j();return}R&&("Tab"===e.key&&!e.shiftKey||"ArrowDown"===e.key)&&(e.preventDefault(),I.current?.querySelector("a, button, input")?.focus())},children:p?(0,t.jsx)(b.IconButton,{ref:P,size:32,alt:D,"aria-expanded":R,"data-analytics-id":"global_sidebar_more_button",children:(0,t.jsx)(s.default,{})}):(0,t.jsx)(g.Button,{ref:P,variant:"ghost",alignment:"start",stretch:!0,text:D,iconLeft:(0,t.jsx)(s.default,{}),"aria-expanded":R,"data-analytics-id":"global_sidebar_more_button"})})})}function P({items:e,hiddenIds:r,save:a,collapsed:n}){let s=(0,l.useIntl)(),d=x(),c=(0,i.useRef)(null),[p,m]=(0,i.useState)(null),h=null!==p;(0,u.useReportSidebarInteractionOpen)(h);let f=()=>{null!==p&&(a(p),m(null))},y=s.formatMessage({id:"home.sidebarNavCustomize",defaultMessage:"Customize"});return(0,t.jsxs)(_.View,{tag:"li",...d,children:[n?(0,t.jsx)(b.IconButton,{ref:c,size:32,alt:y,"aria-expanded":h,onClick:()=>m(r),"data-analytics-id":"global_sidebar_customize_button",children:(0,t.jsx)(o.default,{})}):(0,t.jsx)(g.Button,{ref:c,variant:"ghost",alignment:"start",stretch:!0,text:y,iconLeft:(0,t.jsx)(o.default,{}),"aria-expanded":h,onClick:()=>m(r),"data-analytics-id":"global_sidebar_customize_button"}),null!==p?(0,t.jsx)(S.RawPopover,{isOpen:!0,triggerRef:c,placement:"right top",offset:4,onOpenChange:e=>{e||f()},"data-analytics-id":"global_sidebar_customize_popover",children:(0,t.jsx)(T.Provider,{value:null,children:(0,t.jsx)(A,{items:e,hiddenIds:p,onChange:m,onDone:f})})}):null]})}e.s(["SidebarNavigationCustomization",0,function(e){return(0,R.useFeatureGate)("gate_left_sidebar_configurable_nav_list",!1)?(0,t.jsx)(I,{...e}):(0,t.jsx)(t.Fragment,{children:e.children})},"SidebarNavigationItems",0,function({items:e,collapsed:r}){let a=(0,i.useContext)(T),n=e=>a?.hiddenIds.includes(e.id)&&!e.element.props.isCurrent,s=e.filter(n);return(0,t.jsxs)(t.Fragment,{children:[e.filter(e=>!n(e)).map(e=>(0,i.cloneElement)(e.element,{key:e.id,collapsed:r})),a&&0===s.length?(0,t.jsx)(P,{items:e,hiddenIds:a.hiddenIds,save:a.save,collapsed:r}):null,a&&s.length>0?(0,t.jsx)(O,{items:e,hiddenItems:s,hiddenIds:a.hiddenIds,save:a.save,collapsed:r}):null]})},"useSidebarNavigationContextMenu",0,x])},77440,e=>{e.v({anchor:"SidebarRowContextMenu-module__9uBf1a__anchor",trigger:"SidebarRowContextMenu-module__9uBf1a__trigger"})},257418,e=>{"use strict";var t=e.i(276385),r=e.i(255615),i=e.i(23824),a=e.i(89148),n=e.i(295231),s=e.i(61732),o=e.i(77440);let l=(0,a.cvarsFrom)("SidebarRowContextMenu.module.css",["--x","--y"]);e.s(["SidebarRowContextMenu",0,function({position:e,isOpen:a,onOpenChange:u,triggerLabel:d,children:c}){return(0,t.jsx)(s.View,{clsx:o.default.anchor,style:{[l.x]:`${e.x}px`,[l.y]:`${e.y}px`},children:(0,t.jsxs)(n.PopupMenu,{"data-analytics-id":"sidebar_row_context_menu",isOpen:a,onOpenChange:u,placement:"bottom start",offset:0,trigger:(0,t.jsx)(r.Button,{"aria-label":d,excludeFromTabOrder:!0,className:o.default.trigger}),children:[c,(0,t.jsx)(i.SidebarSectionMoveMenu,{})]})})}])},739832,e=>{e.v({card:"StarterPlanUsageCard-module__GaSrYq__card",upgradeIcon:"StarterPlanUsageCard-module__GaSrYq__upgradeIcon"})},588005,e=>{"use strict";var t=e.i(276385),r=e.i(712903),i=e.i(884214),a=e.i(3466);e.i(214847);var n=e.i(20397),s=e.i(864300),o=e.i(931394),l=e.i(89148),u=e.i(27923),d=e.i(919073),c=e.i(201894),p=e.i(8047),g=e.i(61732),m=e.i(933302),h=e.i(739832);let f={title:"Upgrade your plan",titleId:"home.upgradeYourPlan",description:"Unlock more credits",descriptionId:"home.unlockMoreAgentCredits"},b={control:{badge:f,cta:f},get_100x_credits:{badge:{title:"Get 100x more usage",titleId:"home.upgradeCardGet100xUsage",description:"Unlock more credits",descriptionId:"home.unlockMoreAgentCredits"},cta:{title:"Upgrade for 100x more usage",titleId:"home.upgradeCardUpgrade100xUsage",description:"Get smarter models & more credits when you subscribe.",descriptionId:"home.upgradeCardUpgrade100xUsageSubtitle"}},unlock_smarter_models:{badge:{title:"Use smarter models",titleId:"home.upgradeCardUseSmarterModels",description:"GPT-6 Astra & Claude Fable",descriptionId:"home.upgradeCardAstraFable"},cta:{title:"Upgrade for smarter models",titleId:"home.upgradeCardUpgradeSmarterModels",description:"Access GPT-6 Astra & Claude Fable. Plus get 100x more credits.",descriptionId:"home.upgradeCardAstraFablePlusCredits"}}};function y({usage:e,limit:t}){return t<=0?0:Math.min(1,Math.max(0,e/t))}function S({usage:e}){let r=(0,s.useIntl)(),i=(0,m.useExperimentParam)("sidebar_upgrade_card_treatments","visual_style","control"),n=(0,m.useExperimentParam)("sidebar_upgrade_card_treatments","copy_variant","control");return(0,t.jsx)(v,{usage:e,visualStyle:"control"===i||"blue_fill"===i?i:"control",copyVariant:Object.hasOwn(b,n)?n:"control",renderUpgradeControl:e=>(0,t.jsx)(a.default,{context:"sidebar",content:e,variant:"underlined",stretch:!0,hideCoreIcon:!0,"data-analytics-id":"global_sidebar_starter_plan_upgrade_button"}),renderUpgradeCta:()=>(0,t.jsx)(a.default,{context:"sidebar",variant:"default",colorway:"primary",stretch:!0,hideCoreIcon:!0,text:r.formatMessage({id:"billing.upgradePlan",defaultMessage:"Upgrade plan"}),"data-analytics-id":"global_sidebar_starter_plan_upgrade_button"})})}function v({usage:e,renderUpgradeControl:a,renderUpgradeCta:s,visualStyle:o="control",copyVariant:c="control"}){let m=e.agentCredits,f=m?y(m):0,S="blue_fill"===o&&void 0!==s,R=b[c][S?"cta":"badge"],C=(0,i.useAutoLogView)({elementId:"global_sidebar_starter_plan_usage_card",trackViewport:!0}),T=m&&f>=.75?(0,t.jsx)(_,{label:(0,t.jsx)(n.FormattedMessage,{id:"home.freeAllowance",defaultMessage:"Free allowance"}),usage:m}):null;return S?(0,t.jsx)(g.View,{px:6,py:4,shrink:0,children:(0,t.jsxs)(d.ShadesSurface,{colorShade:"themePrimaryInverted",br:8,border:"regular",p:12,gap:8,innerRef:C,"data-analytics-id":"global_sidebar_starter_plan_usage_card","data-analytics-product-area":"billing",children:[(0,t.jsxs)(g.View,{gap:2,children:[(0,t.jsx)(p.Text,{variant:"text",textAlign:"left",clsx:(0,u.tw)("font-medium whitespace-normal"),children:(0,t.jsx)(n.FormattedMessage,{id:R.titleId,defaultMessage:R.title})}),(0,t.jsx)(p.Text,{variant:"small",color:"dimmer",textAlign:"left",clsx:(0,u.tw)("whitespace-normal"),children:(0,t.jsx)(n.FormattedMessage,{id:R.descriptionId,defaultMessage:R.description})})]}),T,s()]})}):(0,t.jsx)(g.View,{px:6,py:4,shrink:0,children:(0,t.jsx)(d.ShadesSurface,{colorShade:"themePopup",elevate:"1x",border:"regular",br:8,clsx:h.default.card,innerRef:C,"data-analytics-id":"global_sidebar_starter_plan_usage_card","data-analytics-product-area":"billing",children:a((0,t.jsxs)(g.View,{p:12,gap:8,width:"100%",children:[(0,t.jsxs)(g.View,{row:!0,align:"center",justify:"space-between",gap:8,children:[(0,t.jsxs)(g.View,{gap:0,shrink:!0,children:[(0,t.jsx)(p.Text,{variant:"text",textAlign:"left",clsx:(0,u.tw)("font-medium whitespace-normal"),children:(0,t.jsx)(n.FormattedMessage,{id:R.titleId,defaultMessage:R.title})}),(0,t.jsx)(p.Text,{variant:"small",color:"dimmest",textAlign:"left",clsx:(0,u.tw)("whitespace-normal"),children:(0,t.jsx)(n.FormattedMessage,{id:R.descriptionId,defaultMessage:R.description})})]}),(0,t.jsx)(d.ShadesSurface,{colorShade:"themePrimary",elevate:!1,br:8,p:6,clsx:h.default.upgradeIcon,shrink:0,align:"center",justify:"center",children:(0,t.jsx)(r.default,{size:16,color:l.tokens.white})})]}),T]}))})})}function _({label:e,usage:r}){let i=(0,s.useIntl)(),a=y(r),n=i.formatMessage({id:"home.usagePercentUsed",defaultMessage:"{percent}% used"},{percent:Math.floor(100*a)});return(0,t.jsxs)(g.View,{gap:4,children:[(0,t.jsxs)(g.View,{row:!0,align:"center",justify:"space-between",gap:8,children:[(0,t.jsx)(p.Text,{variant:"small",clsx:(0,u.tw)("font-medium whitespace-normal"),children:e}),(0,t.jsx)(p.Text,{variant:"small",color:"dimmer",children:n})]}),(0,t.jsx)(g.View,{"aria-hidden":!0,children:(0,t.jsx)(c.MeasureBar,{total:1,current:a,color:l.tokens.accentPrimaryDefault,backgroundColor:l.tokens.backgroundHigher,tooltipHidden:!0,size:"small"})})]})}e.s(["StarterPlanUsageCard",0,function(){let e=(0,o.useStarterPlanUsage)();return e?(0,t.jsx)(S,{usage:e}):null}])},458495,e=>{"use strict";var t=e.i(602351),r=e.i(19777);let i=(0,t.atom)(null),a=(0,t.atom)(null,(e,t)=>{let r=e(i);return null!==r&&t(i,null),r});e.s(["useHasPendingCreateComposerOutputKind",0,function(){return null!==(0,r.useAtomValue)(i)},"useSetCreateComposerOutputKind",0,function(){return(0,r.useSetAtom)(i)},"useTakeCreateComposerOutputKind",0,function(){return(0,r.useSetAtom)(a)}])},411063,e=>{"use strict";var t=e.i(602351),r=e.i(19777);let i=(0,t.atom)(null);e.s(["useCreateComposerPrompt",0,function(){return(0,r.useAtomValue)(i)},"useSetCreateComposerPrompt",0,function(){return(0,r.useSetAtom)(i)}])},23824,e=>{"use strict";var t=e.i(276385),r=e.i(389959),i=e.i(183035),a=e.i(143524),n=e.i(348867),s=e.i(261647);e.i(214847);var o=e.i(864300),l=e.i(295231);let u=(0,r.createContext)(null);e.s(["SidebarSectionMoveMenu",0,function(){let e=(0,o.useIntl)(),d=(0,r.useContext)(u);if(null===d||0===d.layout.sections.length)return null;let{layout:c,itemKey:p,onMove:g}=d,m=c.sections.find(e=>e.items.some(e=>(0,s.globalSidebarSectionItemKey)(e)===p))?.id,h=c.sections.map(e=>(0,t.jsx)(l.MenuItem,{"data-analytics-id":"sidebar_section_move_menu_item",label:e.name,iconRight:e.id===m?(0,t.jsx)(i.default,{}):null,isDisabled:e.id===m,onAction:()=>g(e.id)},e.id));return void 0!==m&&h.push((0,t.jsx)(l.Separator,{},"remove-separator"),(0,t.jsx)(l.MenuItem,{"data-analytics-id":"sidebar_section_remove_menu_item",icon:(0,t.jsx)(n.default,{}),label:e.formatMessage({id:"globalSidebar.removeFromSection",defaultMessage:"Remove from section"}),onAction:()=>g(null)},"remove")),(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(l.Separator,{}),(0,t.jsx)(l.Submenu,{icon:(0,t.jsx)(a.default,{}),label:e.formatMessage({id:"globalSidebar.moveToSection",defaultMessage:"Move to section"}),children:h})]})},"SidebarSectionMoveMenuContext",0,u])},535230,e=>{"use strict";var t=e.i(276385),r=e.i(389959);let i=(0,r.createContext)(null);e.s(["SidebarInteractionOpenProvider",0,function({children:e,onOpenChange:a}){let[n,s]=(0,r.useState)(0);(0,r.useLayoutEffect)(()=>{a(n>0)},[a,n]);let o=(0,r.useCallback)(()=>{s(e=>e+1)},[]),l=(0,r.useCallback)(()=>{s(e=>Math.max(0,e-1))},[]),u=(0,r.useMemo)(()=>({acquire:o,release:l}),[o,l]);return(0,t.jsx)(i.Provider,{value:u,children:e})},"useReportSidebarInteractionOpen",0,function(e){let t=(0,r.useContext)(i);(0,r.useLayoutEffect)(()=>{if(t&&e)return t.acquire(),()=>t.release()},[t,e])}])},963587,e=>{"use strict";e.s(["SIDEBAR_TOOLTIP_PLACEMENT",0,"top-end"])},457888,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let i={},a=t.gql`
    fragment GlobalSidebarChromeOrg on Org {
  id
  name
  slug
  image
  authorizations {
    viewOrgSecurity {
      isAuthorized
    }
    viewRoutines {
      isAuthorized
    }
  }
  customer {
    ... on Customer {
      id
      settings {
        disableImport
        disableIntegrations
      }
    }
  }
}
    `,n=t.gql`
    query GlobalSidebarChrome($orgSlug: String, $useOrgSlug: Boolean!, $useStoredOrgContext: Boolean!) {
  currentUser {
    timeCreated
    id
    isVerified
    isStaff: hasRole(role: REPLIT_STAFF)
    isExplorer: hasRole(role: EXPLORER)
    username
    firstName
    image
    email
  }
  slugOrg: getOrg(orgSlug: $orgSlug) @include(if: $useOrgSlug) {
    __typename
    ... on Org {
      ...GlobalSidebarChromeOrg
    }
  }
  storedOrg: getUserOrgContext2 @include(if: $useStoredOrgContext) {
    __typename
    ... on Org {
      ...GlobalSidebarChromeOrg
    }
  }
}
    ${a}`;e.s(["useGlobalSidebarChromeQuery",0,function(e){let t={...i,...e};return r.useQuery(n,t)}])},745950,e=>{"use strict";var t=e.i(15801),r=e.i(457888),i=e.i(957667),a=e.i(943633);function n(e,t,r,i){return!!r&&!!i&&(null===t||"Customer"===t.customer.__typename&&t.customer.settings?.[e]===!1)}e.s(["useGlobalSidebarChrome",0,function(){let e=(0,t.useRouter)(),s=function(e,t,r){let n=!r&&("/replEnvironmentDesktop"===t||"/replEnvironmentMobile"===t),s=t.includes("[orgSlug]")||n||r||"/replView"===t,[o]=e.split("?"),l=o.split("/");return s&&"t"===l[1]&&l[2]?{kind:"org",slug:l[2],isWorkspaceRenderer:n}:"/integrations"===t&&""===(0,i.relativeSearchParams)(e).get("orgSlug")||n||!(0,a.supportsSavedOrgContext)(e)?{kind:"personal"}:{kind:"saved"}}(e.asPath,e.pathname,"string"==typeof e.query.conversationId),o="org"===s.kind,l="personal"===s.kind,{data:u,loading:d,error:c}=(0,r.useGlobalSidebarChromeQuery)({variables:{orgSlug:o?s.slug:void 0,useOrgSlug:o,useStoredOrgContext:"saved"===s.kind}}),p=o?u?.slugOrg:u?.storedOrg,g=p?.__typename==="Org"?p:null,m=!c&&(null==p||"Org"===p.__typename),h=l?null:g,f=l||!d,b=l||"saved"===s.kind&&f&&m&&null===h;return{currentUser:u?.currentUser??void 0,isVerified:u?.currentUser?.isVerified===!0,org:h&&{id:h.id,name:h.name,slug:h.slug,image:h.image??null},orgResolved:f,canImport:n("disableImport",h,f,m),canUseIntegrations:n("disableIntegrations",h,f,m),canViewRoutines:b||(h?.authorizations?.viewRoutines?.isAuthorized??!1),canViewSecurity:b||(h?.authorizations?.viewOrgSecurity?.isAuthorized??!1)}}])},333561,e=>{"use strict";var t=e.i(389959);function r(e){return null!==e&&clearTimeout(e),null}class i{manager;chromeElement;expandButtonElement;openTimer;closeTimer;pointerInside;focusInside;interactionOpen;closeWhenInteractionReleases;constructor(e){this.manager=e,this.chromeElement=null,this.expandButtonElement=null,this.openTimer=null,this.closeTimer=null,this.pointerInside=!1,this.focusInside=!1,this.interactionOpen=!1,this.closeWhenInteractionReleases=!1,this.setChromeElement=e=>{this.chromeElement=e},this.setExpandButtonElement=e=>{this.expandButtonElement=e},this.handlePointerEnter=e=>{"mouse"!==e.pointerType||(this.pointerInside=!0,this.closeWhenInteractionReleases=!1,this.closeTimer=r(this.closeTimer),this.manager.isOpen.current||this.manager.isTemporarilyOpen.current||this.interactionOpen||(this.openTimer=r(this.openTimer),this.openTimer=setTimeout(()=>{this.openTimer=null,this.pointerInside&&!this.interactionOpen&&this.manager.openTemporarily()},200)))},this.handlePointerLeave=e=>{"mouse"===e.pointerType&&(this.pointerInside=!1,this.openTimer=r(this.openTimer),this.scheduleClose())},this.handleFocusCapture=()=>{this.focusInside=!this.closeWhenInteractionReleases&&this.manager.isTemporarilyOpen.current&&document.activeElement!==this.expandButtonElement,this.focusInside?this.closeTimer=r(this.closeTimer):this.scheduleClose()},this.handleBlurCapture=e=>{e.currentTarget.contains(e.relatedTarget)||(this.focusInside=!1,this.scheduleClose())},this.handleKeyDownCapture=e=>{"Escape"===e.key&&this.manager.isTemporarilyOpen.current&&!this.interactionOpen&&(this.close(),this.expandButtonElement?.focus())},this.handleInteractionOpenChange=e=>{if(this.interactionOpen=e,e){this.openTimer=r(this.openTimer),this.closeTimer=r(this.closeTimer);return}this.closeWhenInteractionReleases?this.closeAfterInteractionDismissal():this.scheduleClose()},this.handleDocumentPointerDown=e=>{if(!(e.target instanceof Element)||this.chromeElement?.contains(e.target)){this.closeWhenInteractionReleases=!1;return}if(this.focusInside=!1,this.interactionOpen){this.closeWhenInteractionReleases=!0;return}this.close()}}setChromeElement;setExpandButtonElement;handlePointerEnter;handlePointerLeave;handleFocusCapture;handleBlurCapture;handleKeyDownCapture;handleInteractionOpenChange;handleDocumentPointerDown;handleTemporaryClose(){this.closeWhenInteractionReleases=!1,this.focusInside=!1,this.openTimer=r(this.openTimer),this.closeTimer=r(this.closeTimer)}dispose(){this.openTimer=r(this.openTimer),this.closeTimer=r(this.closeTimer)}scheduleClose(){this.openTimer=r(this.openTimer),this.pointerInside||this.focusInside||this.interactionOpen||!this.manager.isTemporarilyOpen.current||(this.closeTimer=r(this.closeTimer),this.closeTimer=setTimeout(()=>{this.closeTimer=null,this.pointerInside||this.focusInside||this.interactionOpen||this.manager.closeTemporarily()},150))}close(){this.closeWhenInteractionReleases=!1,this.openTimer=r(this.openTimer),this.closeTimer=r(this.closeTimer),this.manager.closeTemporarily()}closeAfterInteractionDismissal(){let e=this.chromeElement?.contains(document.activeElement)??!1;this.focusInside=!1,this.close(),e&&this.expandButtonElement?.focus()}}e.s(["useGlobalSidebarHoverPreview",0,function(e,r){let a=(0,t.useMemo)(()=>new i(e),[e]);return(0,t.useEffect)(()=>()=>a.dispose(),[a]),(0,t.useEffect)(()=>r?(document.addEventListener("pointerdown",a.handleDocumentPointerDown,!0),()=>{document.removeEventListener("pointerdown",a.handleDocumentPointerDown,!0)}):void a.handleTemporaryClose(),[a,r]),{chromeRef:a.setChromeElement,expandButtonRef:a.setExpandButtonElement,onPointerEnter:a.handlePointerEnter,onPointerLeave:a.handlePointerLeave,onFocusCapture:a.handleFocusCapture,onBlurCapture:a.handleBlurCapture,onKeyDownCapture:a.handleKeyDownCapture,onInteractionOpenChange:a.handleInteractionOpenChange}}])},769385,e=>{"use strict";var t=e.i(389959),r=e.i(960933),i=e.i(208018),a=e.i(489859);let n=r.Type.Array(r.Type.String()),s=["templates"],o=["projects","routines","library"];e.s(["useSidebarNavigationPreferences",0,function(e){let[r,l]=(0,t.useState)([...s]);return(0,i.default)(()=>{let t,r,i=a.default.get(e,n);l(null===i?[...s]:(t=o.every(e=>i.includes(e)),r=i.filter(e=>!o.includes(e)),t&&!r.includes("things")&&r.push("things"),r))},[e]),{hiddenIds:r,save:(0,t.useCallback)(t=>{a.default.set(e,t),l(t)},[e])}}])},696606,e=>{"use strict";var t=e.i(351623),r=e.i(344480),i=e.i(975473);let a={},n=t.gql`
    query StarterPlanUsageCard {
  currentUser {
    id
    customer {
      id
      authorizations {
        viewStarterPlanUsage {
          isAuthorized
        }
      }
    }
    starterPlanUsage {
      agentCredits {
        usage
        limit
      }
    }
  }
}
    `,s=t.gql`
    query RefreshStarterPlanAgentUsage {
  currentUser {
    id
    starterPlanUsage {
      agentCredits {
        usage
        limit
      }
    }
  }
}
    `;e.s(["StarterPlanUsageCardDocument",0,n,"useRefreshStarterPlanAgentUsageLazyQuery",0,function(e){let t={...a,...e};return i.useLazyQuery(s,t)},"useStarterPlanUsageCardQuery",0,function(e){let t={...a,...e};return r.useQuery(n,t)}])},931394,e=>{"use strict";var t=e.i(389959),r=e.i(696606);e.s(["useRefreshStarterPlanAgentUsage",0,function(){let[e]=(0,r.useRefreshStarterPlanAgentUsageLazyQuery)({fetchPolicy:"network-only"});return e},"useStarterPlanUsage",0,function(){let{data:e}=(0,r.useStarterPlanUsageCardQuery)(),t=e?.currentUser;return t?.customer.authorizations.viewStarterPlanUsage.isAuthorized?{agentCredits:t.starterPlanUsage?.agentCredits??null}:null},"useUpdateStarterPlanAgentUsage",0,function(){let[,{client:e}]=(0,r.useRefreshStarterPlanAgentUsageLazyQuery)();return(0,t.useCallback)((t,i)=>{e.cache.updateQuery({query:r.StarterPlanUsageCardDocument},e=>e?.currentUser?.id===t&&e.currentUser.customer.authorizations.viewStarterPlanUsage.isAuthorized?{...e,currentUser:{...e.currentUser,starterPlanUsage:{__typename:"StarterPlanUsage",agentCredits:{__typename:"StarterPlanAgentCreditUsage",...i}}}}:e)},[e.cache])}])},23930,e=>{"use strict";e.i(242933);let t=new(e.i(790164)).ObservableState(null);e.s(["publishZoomBoardFooter",0,function(e){t.set(e)},"zoomBoardFooterState",0,function(){return t}])},288992,e=>{"use strict";e.i(242933);var t=e.i(790164);let r=new t.ObservableState(null),i=new t.ObservableState(!1);e.s(["publishZoomChromeCollapse",0,function(e){r.set(e)},"publishZoomChromeCollapseEligible",0,function(e){i.set(e)},"zoomChromeCollapseEligibleState",0,function(){return i},"zoomChromeCollapseState",0,function(){return r}])},532295,e=>{"use strict";var t=e.i(276385),r=e.i(196786),i=e.i(389959),a=e.i(269848),n=e.i(195206),s=e.i(875420);e.i(214847);var o=e.i(864300);e.i(450717);var l=e.i(242917),u=e.i(384001),d=e.i(61732);let c=(0,r.default)(()=>e.A(201776).then(e=>e.CluiExecutionFrame),{loadableGenerated:{modules:[312726]},ssr:!1,loading:()=>(0,t.jsx)(d.View,{align:"center",justify:"center",p:16,children:(0,t.jsx)(a.default,{})})});e.s(["useCluiCommand",0,function(e,r){let a=(0,o.useIntl)(),{show:d}=(0,l.useGlobalModal)(),p=a.formatMessage({id:"home.searchCommandsLabel",defaultMessage:"Commands"});return(0,i.useMemo)(()=>({data:{type:"context",icon:(0,t.jsx)(n.default,{}),label:p,key:"clui"},commands:()=>e?.commands?Object.entries(e.commands).map(([e,i])=>(function e(r,i,a,o,l){let d=0===a?s.CluiIconMap[r]??(0,t.jsx)(n.default,{}):(0,t.jsx)(n.default,{}),p=void 0!==i.commands&&Object.keys(i.commands).length>0,g=[...o,r],m=`clui:${g.join("/")}`;if(p)return{match:u.matchLabel,data:{type:"context",label:r,key:m,description:i.description??"",icon:d},commands:t=>t.active||t.searchQuery?Object.entries(i.commands??{}).map(([t,r])=>e(t,r,a+1,g,l)):[]};if(i.access?.isAllowed===!1){let{allowedRoles:e}=i.access;return{match:u.matchLabel,data:{type:"action",label:r,key:m,description:i.description??"",icon:d,isDisabled:!0,run:()=>(l(g.join(" "),e),{type:"keep-open"})}}}return{match:u.matchLabel,data:{type:"context",label:r,key:m,description:i.description??"",icon:d,view:(0,t.jsx)(c,{command:i})},commands:()=>[]}})(e,i,0,[],(e,t)=>{d("CluiCommandAccessModal",{commandPath:e,currentRoles:r,allowedRoles:t})})):[]}),[e,p,r,d])}])},643732,e=>{"use strict";var t=e.i(351623),r=e.i(319801);let i=t.gql`
    fragment GlobalSearchMatchFields on OrgSearchMatch {
  documentKind
  text
  highlightRanges {
    start
    end
  }
}
    `,a=t.gql`
    fragment GlobalSearchResultFields on OrgSearchResult {
  __typename
  ... on OrgSearchReplResult {
    matches {
      ...GlobalSearchMatchFields
    }
    repl {
      id
      title
      description
      timeCreated
      timeUpdated
      ...ReplLinkRepl
    }
  }
  ... on OrgSearchConversationResult {
    matches {
      ...GlobalSearchMatchFields
    }
    conversation {
      id
      slug
      title
      lastActivityAt
    }
  }
  ... on OrgSearchAssetResult {
    matches {
      ...GlobalSearchMatchFields
    }
    asset {
      id
      displayName
      contentType
      timeCreated
      timeUpdated
      source {
        __typename
        ... on Repl {
          id
          replTitle: title
          ...ReplLinkRepl
        }
        ... on Conversation {
          id
          conversationTitle: title
        }
      }
    }
  }
  ... on OrgSearchArtifactResult {
    matches {
      ...GlobalSearchMatchFields
    }
    artifact {
      id
      displayName
      artifactType
      timeCreated
      timeUpdated
      repl {
        id
        title
        ...ReplLinkRepl
      }
    }
  }
}
    ${i}
${r.ReplLinkReplFragmentDoc}`;e.s(["GlobalSearchResultFieldsFragmentDoc",0,a])},97678,e=>{e.v({askAgentFooter:"index-module__MurT_W__askAgentFooter",askAgentIcon:"index-module__MurT_W__askAgentIcon",askAgentLabel:"index-module__MurT_W__askAgentLabel",askAgentQuery:"index-module__MurT_W__askAgentQuery",root:"index-module__MurT_W__root"})},576592,e=>{"use strict";e.s(["GlobalSearch",()=>V,"GlobalSearchPaletteHost",()=>$,"useGlobalSearchState",()=>K]);var t=e.i(276385),r=e.i(15801),i=e.i(389959),a=e.i(908796),n=e.i(96250),s=e.i(403649),o=e.i(709485),l=e.i(946689),u=e.i(151027),d=e.i(295798);e.i(214847);var c=e.i(800686),p=e.i(864300),g=e.i(797265),m=e.i(415541),h=e.i(119474),f=e.i(559357),b=e.i(23818),y=e.i(295621),S=e.i(527709),v=e.i(554256),_=e.i(296561),R=e.i(643484),C=e.i(903790),T=e.i(66742),x=e.i(108431),I=e.i(61732),A=e.i(678852),O=e.i(204977),P=e.i(856919),k=e.i(558407),M=e.i(411063),w=e.i(97678);let E=[a.ResourceKind.Repl,a.ResourceKind.Conversation,a.ResourceKind.Asset,a.ResourceKind.Artifact],j=[],D={...(0,c.defineMessages)({REPL:{id:"home.searchDegradedKindProjects",defaultMessage:"Projects"},ASSET:{id:"home.searchDegradedKindFiles",defaultMessage:"Files"},ARTIFACT:{id:"home.searchDegradedKindArtifacts",defaultMessage:"Artifacts"}}),CONVERSATION:g.CHAT_DISPLAY_NAME.plural},L=(0,c.defineMessages)({projects:{id:"home.searchNoProjectMatches",defaultMessage:'No projects matching "{term}"'},chats:{id:"home.searchNoChatMatches",defaultMessage:'No {entityName} matching "{term}"'},library:{id:"home.searchNoLibraryMatches",defaultMessage:'No library items matching "{term}"'}});function F({kinds:e}){let r=(0,p.useIntl)(),i=e.flatMap(e=>{let t=D[e];return t?[r.formatMessage(t)]:[]});return 0===i.length?null:(0,t.jsx)(x.StatusBanner,{variant:"flush",colorway:"warning",icon:(0,t.jsx)(s.default,{}),text:r.formatMessage({id:"home.searchDegradedKinds",defaultMessage:"Showing partial results — {kinds} are not available right now."},{kinds:r.formatList(i,{type:"conjunction"})})})}function G({children:e,clearCommandsRef:r,onCluiBreadcrumb:a,onCluiBreadcrumbRemoved:n,onProjectBreadcrumbRemoved:s}){let{breadcrumbs:o}=(0,O.useBreadcrumbs)(),l=(0,O.useClearCommands)(),u=(0,i.useRef)(!1),d=(0,i.useRef)(!1),c=o.some(e=>"context"===e.data.type&&e.data.key?.startsWith("clui:")),p=o.some(e=>"context"===e.data.type&&"project-actions"===e.data.key),g=c||p;return(0,i.useEffect)(()=>(r.current=()=>{g&&l()},()=>{r.current=()=>{}}),[l,r,g]),(0,i.useEffect)(()=>{c&&a()},[c,a]),(0,i.useEffect)(()=>{u.current&&!c&&n(),u.current=c},[c,n]),(0,i.useEffect)(()=>{d.current&&!p&&s(),d.current=p},[p,s]),p?(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(I.View,{py:4}),(0,t.jsx)(C.DividerH,{})]}):e}function U({query:e,onSubmit:r}){let i=(0,p.useIntl)(),a=e.trim(),s=i.formatMessage({id:"home.searchAskAgent",defaultMessage:"Ask the agent"}),o=a?i.formatMessage({id:"home.searchAskAgentQuery",defaultMessage:" “{query}”"},{query:a}):null;return(0,t.jsx)(I.View,{clsx:w.default.askAgentFooter,children:(0,t.jsx)(R.Button,{stretch:!0,alignment:"space-between",variant:"listItem",colorway:"blue",size:"2xl",textVariant:"text",iconLeft:(0,t.jsx)(n.default,{clsx:w.default.askAgentIcon,size:16}),secondaryText:(0,t.jsx)(f.KeyComboBlocks,{keyCombo:"Tab",small:!0,horizontalPadding:4}),text:(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)("span",{clsx:w.default.askAgentLabel,children:s}),o?(0,t.jsx)("span",{clsx:w.default.askAgentQuery,children:o}):null]}),"data-analytics-id":"global_search_ask_agent_button","data-analytics-label":"ask_agent",onClick:()=>r("ask_agent_button")})})}let z=[P.toggleGlobalSearchPaletteCommand];function $({dataSource:e,workspace:r}){let i=(0,k.useGlobalSearchPaletteIsOpen)(),a=(0,k.useWorkspaceGlobalSearchPaletteHostActive)();return(0,t.jsx)(h.KeybindingsSurface,{commands:z,children:(0,t.jsxs)(H,{children:[i?(0,t.jsx)(N,{dataSource:e,orgId:r?.id,orgSlug:r?.slug}):null,a?null:(0,t.jsx)(P.default,{})]})})}function H({children:e}){let r=(0,k.useToggleGlobalSearchPalette)("global_shortcut");return(0,b.useCommandBinding)(()=>[P.toggleGlobalSearchPaletteCommand.id,{run:r,bubble:!1}],[r]),(0,t.jsx)(t.Fragment,{children:e})}function N({dataSource:e,orgId:t,orgSlug:r}){let{command:a,cluiCommand:n,footer:s,stackDescriptions:o,subheader:l,onInputValueChange:u}=W({orgId:t,orgSlug:r,renderAsGroup:!0,searchSessionId:(0,k.useGlobalSearchSessionId)(),dataSource:e}),d=(0,k.useProjectCommandLabel)(),c=(0,k.useSetGlobalSearchPalettePresentation)();return(0,P.useCommand)(a,{priority:100,scope:"global"}),(0,P.useCommand)(n,{enabled:null!==d,scope:"clui"}),(0,i.useEffect)(()=>{c({footer:s,groupProjectCommands:!0,subheader:l,onInputValueChange:u,stackDescriptions:o})},[s,u,c,o,l]),(0,i.useEffect)(()=>()=>c({}),[c]),null}function V({onAction:e,workspace:r}){let i=(0,u.useCurrentUserStoredOrgContext)(),a=void 0===r?i.orgId:r?.id,n=void 0===r?i.orgSlug:r?.slug;return(0,t.jsx)(I.View,{clsx:w.default.root,children:(0,t.jsx)(B,{orgId:a,orgSlug:n,onAction:e})})}function B({orgId:e,orgSlug:r,onAction:i}){let{command:a,stackDescriptions:n,subheader:s,onInputValueChange:o}=W({orgId:e,orgSlug:r,searchSessionId:null});return(0,t.jsx)(A.CommandBar,{autoFocus:!0,command:a,subheader:s,onInputValueChange:o,minItemHeight:n?44:void 0,showToolDescriptions:n,stackDescriptions:n,onAction:i})}function W({dataSource:e,orgId:n,orgSlug:s,renderAsGroup:u=!1,searchSessionId:c}){let h,f=(0,r.useRouter)(),b=(0,p.useIntl)(),y=(0,k.useCloseGlobalSearchPalette)(),R=(0,M.useSetCreateComposerPrompt)(),x=(0,l.useBonsaiWebviewQuery)(),A=(0,k.useGlobalSearchPaletteIsOpen)(),[O,P]=(0,k.useGlobalSearchPaletteFilter)(),w=(0,i.useRef)(()=>{}),D=(0,i.useRef)(""),[z,$]=(0,i.useState)(!1),[H,N]=(0,i.useState)(j),V=s?`/t/${encodeURIComponent(s)}`:"/home",B=(0,i.useCallback)(e=>{let t=D.current.trim();if(null!==c&&(0,m.trackV2)(o.eventsV2.GLOBAL_SEARCH_USED,{action:"agent_handoff_selected",handoff_trigger:e,has_prefilled_prompt:!!t,search_session_id:c}),y("agent_handoff"),t){R({text:t}),f.push({pathname:V,query:{...x,create:!0}},V).then(e=>{e||R(null)},()=>R(null));return}f.push({pathname:V,query:{...x,create:!0}})},[y,V,f,R,c,x]);(0,i.useEffect)(()=>{if(!A){D.current="";return}let e=e=>{e.defaultPrevented||"Tab"!==e.key||e.shiftKey||(e.preventDefault(),B("tab"))};return document.addEventListener("keydown",e),()=>document.removeEventListener("keydown",e)},[A,B]);let q=(0,S.useWorkspaceSearchDataSource)(n),K=e??q,Q=b.formatMessage({id:"home.searchProjectsChatsLibrary",defaultMessage:"Search projects, {entityName}, and library"},{entityName:b.formatMessage(g.CHAT_DISPLAY_NAME.pluralLower)}),Y=(0,i.useMemo)(()=>[{value:"all",label:b.formatMessage({id:"home.searchFilterAll",defaultMessage:"All"})},{value:"projects",label:b.formatMessage({id:"home.searchFilterProjects",defaultMessage:"Projects"})},{value:"chats",label:b.formatMessage(g.CHAT_DISPLAY_NAME.plural)},{value:"library",label:b.formatMessage({id:"home.searchFilterLibrary",defaultMessage:"Library"})}],[b]),Z=(0,i.useMemo)(()=>"library"===O?[a.ResourceKind.Asset,a.ResourceKind.Artifact]:"projects"===O?[a.ResourceKind.Repl]:"chats"===O?[a.ResourceKind.Conversation]:E,[O]),X="project"===O||"clui"===O?"all":O;"projects"===O?h=b.formatMessage({id:"home.searchNoRecentProjects",defaultMessage:"No recent projects"}):"chats"===O?h=b.formatMessage({id:"home.searchNoRecentChats",defaultMessage:"No recent {entityName}"},{entityName:b.formatMessage(g.CHAT_DISPLAY_NAME.pluralLower)}):"library"===O&&(h=b.formatMessage({id:"home.searchNoRecentLibraryItems",defaultMessage:"No recent library items"}));let J=(0,_.useGlobalSearchResources)(K,Z,!z,null===c?null:X,c),ee=J.search,et=(0,d.default)(Z),er=(0,i.useCallback)(async e=>{let t=()=>{let t=et.current;return e.query===D.current&&e.resourceKinds.length===t.length&&e.resourceKinds.every((e,r)=>e===t[r])},r=await ee({...e,isCurrent:t});return t()&&("success"===r.status?N(r.degradedKinds.length>0?r.degradedKinds:j):"failure"===r.status&&N(j)),r},[ee,et]);(0,i.useEffect)(()=>{N(j)},[Z]);let ei="success"===J.recent.status?J.recent.degradedKinds:j,ea=z?H:ei,en="clui"!==O&&"project"!==O;(0,i.useEffect)(()=>()=>P("all"),[P]);let es=(0,i.useCallback)(()=>{P("clui")},[P]),eo=(0,i.useCallback)(()=>{P("all")},[P]),el=(0,i.useCallback)(e=>{e!==O&&(w.current(),null!==c&&"project"!==e&&"clui"!==e&&(0,m.trackV2)(o.eventsV2.GLOBAL_SEARCH_USED,{action:"filter_selected",filter:e,search_session_id:c}),P(e))},[O,c,P]),eu=(0,i.useMemo)(()=>K?(0,t.jsx)(G,{clearCommandsRef:w,onCluiBreadcrumb:es,onCluiBreadcrumbRemoved:eo,onProjectBreadcrumbRemoved:eo,children:(0,t.jsxs)(t.Fragment,{children:[(0,t.jsx)(I.View,{px:8,py:6,row:!0,wrap:!0,align:"center",justify:"space-between",gap:8,children:(0,t.jsx)(I.View,{row:!0,wrap:!0,gap:4,role:"group","aria-label":b.formatMessage({id:"home.searchFilterAriaLabel",defaultMessage:"Filter search results"}),children:Y.map(({value:e,label:r})=>(0,t.jsx)(T.PillButton,{id:`global-search-filter-${e}`,"data-analytics-id":`global_search_filter_${e}_chip`,"data-analytics-label":e,colorway:O===e?"blue":void 0,"aria-pressed":O===e,onClick:()=>el(e),text:r},e))})}),(0,t.jsx)(C.DividerH,{}),"clui"!==O&&ea.length>0?(0,t.jsx)(F,{kinds:ea}):null]})}):void 0,[K,eo,ea,O,Y,el,b,es]),ed=(0,i.useCallback)(e=>(0,t.jsx)(U,{query:e,onSubmit:B}),[B]),ec=(0,i.useCallback)(e=>{D.current=e,$(""!==e.trim()),N(j)},[]),ep=(0,v.useOrgActionsConfig)({orgId:n,orgSlug:s});return{command:(0,v.useUnifiedResourceCommand)({orgSlug:s,placeholder:Q,presentation:u?"group":"context",recent:J.recent,resourceFilter:X,search:er,selectedResourceKinds:Z,showQuickActions:"all"===O,searchSessionId:c,routinesEnabled:ep.routinesEnabled,designSystemsEnabled:ep.designSystemsEnabled,emptyLabel:h,emptySearchMessage:L[O]}),cluiCommand:ep.command,footer:K?ed:void 0,stackDescriptions:en,subheader:eu,onInputValueChange:ec}}let q=(0,y.defaultKeyCombo)({cmdOrCtrl:!0,key:"k"});function K(){var e;let[t,r]=(0,i.useState)(!1);return e=(0,i.useCallback)(()=>r(e=>!e),[r]),(0,i.useEffect)(()=>{let t=t=>{(0,y.getKeyCombination)(t)===q&&e()};return document.addEventListener("keydown",t),()=>{document.removeEventListener("keydown",t)}},[e]),[t,r]}},280839,e=>{"use strict";var t=e.i(351623),r=e.i(643732);e.i(344480);var i=e.i(975473);let a={},n=t.gql`
    query GlobalWorkspaceResourceSearch($orgId: String, $query: String, $resourceKinds: [ResourceKind!]!, $count: Int!, $searchRequestId: String) {
  workspaceSearch(
    orgId: $orgId
    query: $query
    resourceKinds: $resourceKinds
    count: $count
    searchRequestId: $searchRequestId
  ) {
    __typename
    ... on OrgSearchConnection {
      degradedKinds
      items {
        ...GlobalSearchResultFields
      }
    }
  }
}
    ${r.GlobalSearchResultFieldsFragmentDoc}`;e.s(["useGlobalWorkspaceResourceSearchLazyQuery",0,function(e){let t={...a,...e};return i.useLazyQuery(n,t)}])},527709,e=>{"use strict";var t=e.i(389959),r=e.i(280839);e.s(["useWorkspaceSearchDataSource",0,function(e){let[i]=(0,r.useGlobalWorkspaceResourceSearchLazyQuery)({fetchPolicy:"network-only"});return(0,t.useMemo)(()=>{async function t(t,r,a,n,s){try{let{data:o}=await i({variables:{orgId:e,query:t,resourceKinds:[...r],count:a,searchRequestId:s},context:{queryDeduplication:!1,noBatch:!0,fetchOptions:{signal:n}}}),l=o?.workspaceSearch;if(!l||"UnauthorizedError"===l.__typename||"ServiceUnavailable"===l.__typename||"UserError"===l.__typename||l.degradedKinds.length>=r.length)return{status:"failure"};return{status:"success",items:l.items,degradedKinds:l.degradedKinds}}catch{return n.aborted?{status:"cancelled"}:{status:"failure"}}}return{recent:({resourceKinds:e,count:r,signal:i})=>t(void 0,e,r,i),search:({query:e,resourceKinds:r,count:i,signal:a,searchRequestId:n})=>t(e,r,i,a,n)}},[i,e])}])},939052,e=>{"use strict";var t=e.i(921125);function r(e,r){let i="1"===r.mobileWebview||"true"===r.mobileWebview,a="1"===r.tabletWorkspace||"true"===r.tabletWorkspace,n=e.nextPagePathname;a&&"/replEnvironmentMobile"===n?n="/replEnvironmentDesktop":!a&&i&&"/replEnvironmentDesktop"===n&&(n="/replEnvironmentMobile");let{href:s,as:o}=(0,t.replLinkProps)({...e,nextPagePathname:n});return{href:{...s,query:{...r,...s.query}},as:{...o,query:{...o.query,...r}}}}e.s(["globalSearchItemDestination",0,function(e,{conversationHref:t,orgSlug:i,webviewQuery:a={}}){switch(e.__typename){case"OrgSearchReplResult":return r(e.repl,a);case"OrgSearchConversationResult":return t(e.conversation.id,i);case"OrgSearchAssetResult":return"Repl"===e.asset.source.__typename?r(e.asset.source,a):t(e.asset.source.id,i);case"OrgSearchArtifactResult":return r(e.artifact.repl,a)}}])},715365,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let i={},a=t.gql`
    query GlobalOrgActions($orgSlug: String!) {
  currentUser {
    id
    clui
    isVerified
    personalOrgAuthorizations {
      ... on OrgAuthorizations {
        viewRoutines {
          isAuthorized
        }
        viewArtifactTemplates {
          isAuthorized
        }
      }
    }
    roles(only: [ADMIN, SUPPORT, DEVELOPER, BILLING_ADMIN, SALES, TRUST_AND_SAFETY]) {
      id
      key
    }
  }
  getOrg(orgSlug: $orgSlug) {
    ... on Org {
      id
      authorizations {
        viewRoutines {
          isAuthorized
        }
        viewArtifactTemplates {
          isAuthorized
        }
      }
    }
  }
}
    `;e.s(["useGlobalOrgActionsQuery",0,function(e){let t={...i,...e};return r.useQuery(a,t)}])},345367,e=>{e.v({resultText:"unifiedCommands-module__Jyn0eq__resultText"})},554256,e=>{"use strict";e.s(["useOrgActionsConfig",()=>ei,"useUnifiedResourceCommand",()=>er]);var t=e.i(276385),r=e.i(15801),i=e.i(389959),a=e.i(908796),n=e.i(715365),s=e.i(940322),o=e.i(802929),l=e.i(917255),u=e.i(96250),d=e.i(429662),c=e.i(189414),p=e.i(61935),g=e.i(927225),m=e.i(109591),h=e.i(40916),f=e.i(357253),b=e.i(652830),y=e.i(76112),S=e.i(394572),v=e.i(612343),_=e.i(709485),R=e.i(662297),C=e.i(66924),T=e.i(946689),x=e.i(44477),I=e.i(306229),A=e.i(295798);e.i(214847);var O=e.i(614852),P=e.i(864300),k=e.i(797265),M=e.i(776065),w=e.i(415541),E=e.i(987520),j=e.i(174474),D=e.i(313287),L=e.i(384001),F=e.i(532295),G=e.i(939052),U=e.i(7579),z=e.i(89148),$=e.i(8047),H=e.i(61732);e.i(856919);var N=e.i(558407),V=e.i(618457),B=e.i(733065),W=e.i(345367);function q(){return(0,t.jsx)(m.default,{size:16,color:z.tokens.foregroundDimmest})}let K=Number.MAX_SAFE_INTEGER,Q=Array.from({length:5},(e,r)=>({data:{type:"output",key:`recent-skeleton-${r}`,label:"",icon:(0,t.jsx)(t.Fragment,{}),isInteractive:!1,stackDescription:!0,skeletonIndex:r},match:()=>({score:1})}));function Y({timestamp:e}){let r=(0,P.useIntl)();return(0,t.jsx)($.Text,{variant:"codeSmall",color:"dimmest",multiline:!1,children:(0,O.formatRelativeTime)(e,{locale:r.locale,style:"narrow"})})}function Z(e,t,r){return e.formatMessage({id:"home.searchFileDescription",defaultMessage:"{type} • from {source}"},{type:t,source:r})}function X(e){return e?.highlightRanges.map(({start:e,end:t})=>({from:e,to:t}))??[]}function J({active:e=!1,description:r,descriptionHighlight:i,icon:a,label:n,labelHighlight:s}){return(0,t.jsxs)(H.View,{row:!0,align:"center",gap:8,px:8,py:6,width:"100%",children:[(0,t.jsx)(H.View,{width:16,height:16,shrink:0,children:a}),(0,t.jsxs)(H.View,{clsx:W.default.resultText,grow:!0,shrink:!0,children:[(0,t.jsx)(V.HighlightMatches,{dimmed:!e,highlight:s,text:n}),r?(0,t.jsx)(V.HighlightMatches,{colorOverride:e?z.tokens.foregroundDimmer:z.tokens.foregroundDimmest,dimmed:!e,highlight:i,text:r,variant:"small"}):null]})]})}function ee(e,{conversationHref:r,filter:i,intl:n,matchScore:s=1,orgSlug:l,position:d,router:c,searchSessionId:p,showRecency:g=!1,source:m,untitledChatLabel:h,webviewQuery:f}){let b,y,S,v,_;switch(e.__typename){case"OrgSearchReplResult":b=`repl:${e.repl.id}`,y=e.repl.title,S=e.repl.description||n.formatMessage({id:"home.replNounSingular",defaultMessage:"Project"}),v=e.repl.timeUpdated??e.repl.timeCreated,_=(0,t.jsx)(q,{});break;case"OrgSearchConversationResult":b=`conversation:${e.conversation.id}`,y=e.conversation.title??h,S=n.formatMessage(k.CHAT_DISPLAY_NAME.singular),v=e.conversation.lastActivityAt,_=(0,t.jsx)(u.default,{});break;case"OrgSearchAssetResult":{b=`asset:${e.asset.id}`,y=e.asset.displayName;let r="Repl"===e.asset.source.__typename?e.asset.source.replTitle:e.asset.source.conversationTitle??h,i=(0,D.fileTypeFromContentType)(e.asset.contentType??""),a=(0,D.getFileOutputConfigForFile)(i,{filePath:e.asset.displayName,contentType:e.asset.contentType});S=Z(n,n.formatMessage({id:a.labelId,defaultMessage:a.label}),r),v=e.asset.timeUpdated??e.asset.timeCreated,_=(0,t.jsx)(D.FileOutputIcon,{type:i,path:e.asset.displayName,contentType:e.asset.contentType});break}case"OrgSearchArtifactResult":{b=`artifact:${e.artifact.id}`,y=e.artifact.displayName;let r=(0,C.getArtifactKindConfigFromString)(E.ARTIFACT_KIND_BY_TYPE[e.artifact.artifactType]);S=Z(n,r.labelIntlId?n.formatMessage({id:r.labelIntlId,defaultMessage:r.label}):r.label,e.artifact.repl.title),v=e.artifact.timeUpdated??e.artifact.timeCreated,_=(0,t.jsx)(o.default,{})}}let T=e.matches.find(e=>e.documentKind===a.OrgSearchMatchDocumentKind.Title&&e.text===y),x=e.matches.find(e=>e.documentKind!==a.OrgSearchMatchDocumentKind.Title&&e.text);x&&(S=x.text);let I=X(T),A=X(x),O=I.length>0||A.length>0?{height:48,content:(0,t.jsx)(J,{description:S,descriptionHighlight:A,icon:_,label:y,labelHighlight:I})}:void 0,P={type:"action",key:b,label:y,description:S,stackDescription:!0,trailingIcon:g&&v?(0,t.jsx)(Y,{timestamp:new Date(v).getTime()}):void 0,icon:_},M=(0,G.globalSearchItemDestination)(e,{conversationHref:r,orgSlug:l,webviewQuery:f});return{data:{...P,run:t=>{t&&null!==p&&(0,R.trackGlobalSearchResourceSelection)(e,{event:t,filter:i,position:d,searchSessionId:p,source:m}),c.push(M.href,M.as)}},match:()=>({score:s,render:O})}}function et(e){return{data:{type:"error",label:e,icon:(0,t.jsx)(t.Fragment,{}),isInteractive:!1},match:()=>({score:K})}}function er({orgSlug:e,placeholder:a,presentation:n,recent:u,resourceFilter:m,search:C,selectedResourceKinds:x,showQuickActions:O,searchSessionId:E,routinesEnabled:D,designSystemsEnabled:F,emptyLabel:G,emptySearchMessage:z}){let $=(0,r.useRouter)(),H=(0,N.useCloseGlobalSearchPalette)(),V=(0,P.useIntl)(),W=(0,B.useGlobalSidebarOrNull)(),Y=(0,U.useGlobalThemeCommand)(),Z=(0,T.useBonsaiWebviewQuery)(),X=(0,T.useConversationWorkspaceHref)(),J=(0,A.default)(x),ei=V.formatMessage(k.CHAT_DISPLAY_NAME.singularLower),ea=V.formatMessage(k.CHAT_DISPLAY_NAME.pluralLower),en=V.formatMessage({id:"home.searchUntitledChat",defaultMessage:"Untitled {entityName}"},{entityName:ei}),es=V.formatMessage({id:"home.searchUnavailable",defaultMessage:"Search is unavailable right now"}),eo=V.formatMessage({id:"home.searchRecent",defaultMessage:"Recent"}),el=V.formatMessage({id:"home.searchYourWork",defaultMessage:"Your work"}),eu=V.formatMessage({id:"home.searchNavigation",defaultMessage:"Navigation"}),ed=V.formatMessage({id:"home.navSettingsLabel",defaultMessage:"Settings"}),ec=V.formatMessage({id:"home.searchActions",defaultMessage:"Actions"}),ep=(0,i.useMemo)(()=>{let r=e?`/t/${encodeURIComponent(e)}`:"/home",i=new URLSearchParams(Z).toString(),a=t=>{let r;return r=e?`/t/${encodeURIComponent(e)}/${t}`:`/${t}`,`${r}${i?`?${i}`:""}`},n=(e,r,i,a)=>({label:e,description:r,icon:a,trailingIcon:(0,t.jsx)(s.default,{size:14}),run:()=>void $.push(i)}),u=(e,t,r,i,a)=>{let s=n(e,t,r,i);return{...s,run:e=>(null!==E&&(e&&(0,w.trackV2)(_.eventsV2.GLOBAL_SEARCH_USED,{action:"page_selected",interaction_method:(0,R.interactionMethod)(e),page:a,search_session_id:E}),H("page_selected")),s.run(e))}},g=(r,i,a,n)=>({label:r,description:i,icon:n,trailingIcon:(0,t.jsx)(s.default,{size:14}),run:()=>(0,M.updatePathWithQueryParams)({router:$,params:(0,j.settingsQueryParams)(a,$.pathname.includes("[orgSlug]")?void 0:e??null)})}),m=[u(V.formatMessage({id:"home.searchProjectsPage",defaultMessage:"Projects"}),V.formatMessage({id:"home.searchProjectsPageDescription",defaultMessage:"Every project in this workspace"}),a("repls"),(0,t.jsx)(q,{}),"projects"),u(V.formatMessage({id:"home.searchLibraryPage",defaultMessage:"Library"}),V.formatMessage({id:"home.searchLibraryPageDescription",defaultMessage:"Docs, sheets, and files your work produced"}),a("library"),(0,t.jsx)(o.default,{}),"library")];D&&m.push(u(V.formatMessage({id:"home.searchRoutinesPage",defaultMessage:"Routines"}),V.formatMessage({id:"home.searchRoutinesPageDescription",defaultMessage:"Manage agents that run on a schedule"}),a("routines"),(0,t.jsx)(d.default,{}),"routines"));let C=[...m];e&&m.push(u(V.formatMessage({id:"home.navSecurityLabel",defaultMessage:"Security"}),V.formatMessage({id:"home.searchSecurityPageDescription",defaultMessage:"Review workspace security findings and controls"}),a("security"),(0,t.jsx)(f.default,{}),"security"),u(V.formatMessage({id:"home.navGroupsLabel",defaultMessage:"Groups"}),V.formatMessage({id:"home.searchGroupsPageDescription",defaultMessage:"Manage workspace groups and permissions"}),a("groups"),(0,t.jsx)(v.default,{}),"groups"));let T=u(V.formatMessage({id:"home.searchNewChatPage",defaultMessage:"New {entityName}"},{entityName:ei}),V.formatMessage({id:"home.searchNewChatPageDescription",defaultMessage:"Start something fresh from the home composer"}),{pathname:r,query:{...Z,create:!0}},(0,t.jsx)(h.default,{}),"new_chat"),x={...T,run:e=>((0,I.clearHomeSidebarDestination)(),T.run(e))},A=[n(V.formatMessage({id:"home.searchImportAction",defaultMessage:"Import to Replit"}),V.formatMessage({id:"home.searchImportActionDescription",defaultMessage:"Bring in code or a design from another tool"}),"/import",(0,t.jsx)(S.default,{})),{label:V.formatMessage({id:"home.toggleSidebar",defaultMessage:"Toggle sidebar"}),description:V.formatMessage({id:"home.searchToggleSidebarDescription",defaultMessage:"Show or hide the main navigation"}),icon:(0,t.jsx)(b.default,{}),run:()=>W?.toggle()},x],O=[g(V.formatMessage({id:"home.searchSettingsUsage",defaultMessage:"Settings → Usage"}),V.formatMessage({id:"home.searchSettingsUsageDescription",defaultMessage:"View spending and resource usage"}),e?"workspaceUsage":"customerUsage",(0,t.jsx)(y.default,{})),g(V.formatMessage({id:"home.searchSettingsIntegrations",defaultMessage:"Settings → Integrations"}),V.formatMessage({id:"home.searchSettingsIntegrationsDescription",defaultMessage:"Manage connected services"}),"integrations",(0,t.jsx)(p.default,{}))];return F&&O.push(g(V.formatMessage({id:"home.searchSettingsDesignSystems",defaultMessage:"Settings → Design systems"}),V.formatMessage({id:"home.searchSettingsDesignSystemsDescription",defaultMessage:"Manage reusable styles and components"}),"designSystems",(0,t.jsx)(c.default,{}))),O.push(g(V.formatMessage({id:"home.searchSettingsCustomization",defaultMessage:"Settings → Customization"}),V.formatMessage({id:"home.searchSettingsCustomizationDescription",defaultMessage:"Manage skills, instructions, and memory"}),"knowledge",(0,t.jsx)(l.default,{}))),{navigation:m,preview:[...C,x],settings:O,actions:A}},[ei,F,W,V,e,$,D,E,H,Z]);return(0,i.useMemo)(()=>{let r={data:{type:"context",label:eo,icon:(0,t.jsx)(t.Fragment,{})},commands:r=>r.searchQuery?[]:"loading"===u.status?Q:"cancelled"===u.status?[]:"failure"===u.status?[et(es)]:0===u.items.length&&G?[{data:{type:"output",label:G,icon:(0,t.jsx)(t.Fragment,{}),isInteractive:!1},match:()=>({score:K})}]:u.items.map((t,r)=>ee(t,{conversationHref:X,filter:m,intl:V,orgSlug:e,position:r+1,router:$,searchSessionId:E,showRecency:!0,source:{result_source:"recent"},untitledChatLabel:en,webviewQuery:Z}))},i=e=>e.map(e=>({data:{type:"action",stackDescription:!0,...e},match:L.matchLabel})),s={data:{type:"context",label:eu,icon:(0,t.jsx)(t.Fragment,{})},commands:()=>i(ep.navigation)},o=i(ep.settings);Y&&o.push(Y);let l={data:{type:"context",label:ed,icon:(0,t.jsx)(t.Fragment,{})},commands:()=>o},d={data:{type:"context",label:ec,icon:(0,t.jsx)(t.Fragment,{})},commands:()=>i(ep.actions)},c={data:{type:"context",label:V.formatMessage({id:"home.searchMoreActions",defaultMessage:"More actions"}),description:V.formatMessage({id:"home.searchMoreActionsDescription",defaultMessage:"Browse navigation, settings, and app actions"}),icon:(0,t.jsx)(g.default,{}),stackDescription:!0},match:L.matchLabel,commands:({active:e})=>e?[s,l,d]:[]},p={data:{type:"context",label:ec,icon:(0,t.jsx)(t.Fragment,{})},commands:()=>[...i(ep.preview),c]},h={data:{type:"context",key:x.join(","),label:el,icon:(0,t.jsx)(t.Fragment,{})},commands:async r=>{if(!r.searchQuery.trim())return[];let i=await C({query:r.searchQuery,resourceKinds:J.current,count:20});return"cancelled"===i.status?[]:"failure"===i.status?[et(es)]:0===i.items.length?[{data:{type:"output",label:V.formatMessage(z??{id:"home.searchNoResourceMatches",defaultMessage:'No projects, {entityName}, or library items matching "{term}"'},{entityName:ea,term:r.searchQuery}),icon:(0,t.jsx)(t.Fragment,{}),isInteractive:!1},match:()=>({score:K})}]:i.items.map((t,r)=>ee(t,{conversationHref:X,filter:m,intl:V,matchScore:K,orgSlug:e,position:r+1,router:$,searchSessionId:E,source:{result_source:"search",search_id:i.searchId},untitledChatLabel:en,webviewQuery:Z}))}};return{data:"group"===n?{type:"group",key:"global-search"}:{type:"context",label:V.formatMessage({id:"home.searchApps",defaultMessage:"Search Apps"}),description:a,icon:(0,t.jsx)(t.Fragment,{})},commands:()=>[r,...O?[p]:[],h]}},[ec,ea,X,G,z,V,eu,e,a,n,ep,u,eo,m,el,$,C,x,J,ed,O,Y,E,en,es,Z])}function ei({orgId:e,orgSlug:t}){let{data:r,error:a}=(0,n.useGlobalOrgActionsQuery)({variables:{orgSlug:t??""},fetchPolicy:"cache-and-network",ssr:!1}),s=r?.currentUser,o=r?.getOrg.__typename==="Org"&&(void 0===e||r.getOrg.id===e)?r.getOrg:null,l=s?.personalOrgAuthorizations.__typename==="OrgAuthorizations"?s.personalOrgAuthorizations:null,u=t?o?.authorizations:l,d=void 0===a&&void 0!==s&&null!=u,c=d&&s?.isVerified===!0&&u?.viewRoutines.isAuthorized===!0,p=d&&u?.viewArtifactTemplates.isAuthorized===!0,g=(0,F.useCluiCommand)(s?.clui,(0,x.getCurrentAdminCluiRoles)(s?.roles));return{command:(0,i.useMemo)(()=>({data:{type:"group",key:"workspace-actions"},commands:()=>[g]}),[g]),routinesEnabled:c,designSystemsEnabled:p}}},296561,e=>{"use strict";var t=e.i(389959),r=e.i(912206),i=e.i(709485),a=e.i(415541);let n={status:"loading"};e.s(["useGlobalSearchResources",0,function(e,s,o,l,u){let d=s.join(","),[c,p]=(0,t.useState)({resourceKindsKey:d,result:n}),g=(0,t.useRef)(null);(0,t.useEffect)(()=>{if(!o)return;let t=new AbortController;return p({resourceKindsKey:d,result:n}),e.recent({resourceKinds:s,count:5,signal:t.signal}).then(e=>{t.signal.aborted||p({resourceKindsKey:d,result:e})}),()=>t.abort()},[e,o,s,d]),(0,t.useEffect)(()=>(g.current?.controller.abort(),g.current=null,()=>{g.current?.controller.abort(),g.current=null}),[e]);let m=(0,t.useCallback)(({query:t,resourceKinds:n,count:s,isCurrent:o})=>{if(!t.trim())return g.current?.controller.abort(),g.current=null,Promise.resolve({status:"success",items:[],degradedKinds:[],searchId:(0,r.v4)()});let d=`${t}\0${n.join(",")}\0${s}`,c=g.current;if(c?.key===d)return c.promise;c?.controller.abort();let p=new AbortController,m=(0,r.v4)(),h=(async()=>{var r;if(!await (r=p.signal,new Promise(e=>{if(r.aborted)return void e(!1);let t=setTimeout(()=>{r.removeEventListener("abort",i),e(!0)},200);function i(){clearTimeout(t),e(!1)}r.addEventListener("abort",i,{once:!0})})))return{status:"cancelled",searchId:m};let d=await e.search({query:t,resourceKinds:n,count:s,searchRequestId:m,signal:p.signal});return null!==l&&null!==u&&o()&&g.current?.searchId===m&&function(e,t,r,n){if("cancelled"!==e.status){if("success"===e.status)return(0,a.trackV2)(i.eventsV2.GLOBAL_SEARCH_USED,{action:"search_performed",filter:t,is_degraded:e.degradedKinds.length>0,result_count:e.items.length,search_id:n,search_session_id:r,status:"success"});(0,a.trackV2)(i.eventsV2.GLOBAL_SEARCH_USED,{action:"search_performed",filter:t,search_id:n,search_session_id:r,status:"failure"})}}(d,l,u,m),{...d,searchId:m}})();return g.current={key:d,controller:p,searchId:m,promise:h},h},[e,u,l]);return{recent:c.resourceKindsKey===d?c.result:n,search:m}}])},7579,e=>{"use strict";var t=e.i(276385),r=e.i(389959),i=e.i(752539),a=e.i(393428),n=e.i(255701),s=e.i(632350);e.i(214847);var o=e.i(864300),l=e.i(384001),u=e.i(401036),d=e.i(841114),c=e.i(375581),p=e.i(665061);e.s(["useGlobalThemeCommand",0,function(){let e=(0,o.useIntl)(),g=(0,s.default)(),{currentTheme:m}=(0,u.useTheme)(),{setActiveTheme:h,isSystemTheme:f}=(0,d.useThemePreference)();return(0,r.useMemo)(()=>g?null:{match:(0,l.createFzfMatchLabel)({defaultScore:p.commandDefaultScores.settings,matchScoreMultiplier:p.commandMatchScoreMultipliers.settings}),data:{type:"context",label:e.formatMessage({id:"workspace.themeCommandLabel",defaultMessage:"Theme"}),icon:(0,t.jsx)(a.default,{}),description:e.formatMessage({id:"workspace.themeCommandDescription",defaultMessage:"Change Replit Theme"})},commands:({active:r})=>{if(!r)return[];let a=c.allOfficialThemes.map(r=>{let a=!f&&m.id===r.id;return{match:(0,l.createFzfMatchLabel)({defaultScore:p.commandDefaultScores.settings,matchScoreMultiplier:p.commandMatchScoreMultipliers.settings}),data:{type:"action",label:r.name,icon:a?(0,t.jsx)(i.default,{}):(0,t.jsx)(n.default,{opacity:0}),description:e.formatMessage({id:"workspace.useThemeDescription",defaultMessage:"Use {name} theme{isCurrent, select, yes { (current)} other {}}"},{name:r.name,isCurrent:a?"yes":"no"}),run:()=>h(r.id)}}});return a.push({match:(0,l.createFzfMatchLabel)({defaultScore:p.commandDefaultScores.settings,matchScoreMultiplier:p.commandMatchScoreMultipliers.settings}),data:{type:"action",label:e.formatMessage({id:"workspace.systemThemeLabel",defaultMessage:"System"}),icon:f?(0,t.jsx)(i.default,{}):(0,t.jsx)(n.default,{opacity:0}),description:e.formatMessage({id:"workspace.systemThemeDescription",defaultMessage:"Follow operating system theme{isCurrent, select, yes { (current)} other {}}"},{isCurrent:f?"yes":"no"}),run:()=>h("system")}}),a}},[m.id,e,g,f,h])}])},93837,e=>{"use strict";var t=e.i(276385),r=e.i(389959);let i=(0,r.createContext)("exited"),a=(0,r.createContext)(null);e.s(["HomeBlankSlateProvider",0,function({children:e}){let[n,s]=(0,r.useState)("exited"),o=(0,r.useCallback)(e=>{s(t=>e?"entered":"exited"===t?"exited":"exiting")},[]);return(0,r.useEffect)(()=>{if("exiting"!==n)return;let e=window.setTimeout(()=>s("exited"),420);return()=>window.clearTimeout(e)},[n]),(0,t.jsx)(a.Provider,{value:o,children:(0,t.jsx)(i.Provider,{value:n,children:e})})},"useHomeBlankSlatePhase",0,function(){return(0,r.useContext)(i)},"useSyncHomeBlankSlateVisibility",0,function(e){let t=(0,r.useContext)(a);(0,r.useEffect)(()=>(t?.(e),()=>t?.(!1)),[t,e])}])},280729,e=>{"use strict";var t=e.i(351623),r=e.i(913864),i=e.i(260666),a=e.i(344480);e.i(975473);let n={},s=t.gql`
    fragment SidebarRecentRepl on Repl {
  id
  title
  iconUrl
  url
  nextPagePathname
  ...ReplEnvironmentDesktopRepl
  ...DeployRepl
}
    ${r.ReplEnvironmentDesktopReplFragmentDoc}
${i.DeployReplFragmentDoc}`,o=t.gql`
    query SidebarRecentRepls($count: Int!) {
  allRecentRepls: recentRepls(count: $count) {
    id
    ...SidebarRecentRepl
  }
}
    ${s}`,l=t.gql`
    query SidebarRecentOrgRepls($count: Int!, $orgId: String) {
  currentUser {
    id
    org(orgId: $orgId) {
      ... on Org {
        id
        recentRepls(input: {count: $count}) {
          items {
            id
            ...SidebarRecentRepl
          }
        }
      }
      ... on Error {
        message
      }
    }
  }
}
    ${s}`;e.s(["useSidebarRecentOrgReplsQuery",0,function(e){let t={...n,...e};return a.useQuery(l,t)},"useSidebarRecentReplsQuery",0,function(e){let t={...n,...e};return a.useQuery(o,t)}])},345836,e=>{"use strict";var t=e.i(276385),r=e.i(562203),i=e.i(389959),a=e.i(280729),n=e.i(269848),s=e.i(780902);e.i(214847);var o=e.i(864300),l=e.i(797265),u=e.i(27923),d=e.i(965531),c=e.i(295231),p=e.i(8047),g=e.i(61732),m=e.i(921125);let h=({repl:e})=>{let r=(0,m.replLinkProps)(e);return(0,t.jsx)(c.MenuItem,{label:e.title,href:r.href,as:r.as})},f=({loading:e,recentRepls:r,links:i})=>{let a,s=(0,o.useIntl)(),d=s.formatMessage(l.REPL_DISPLAY_NAME.plural),m=i?.length?(0,t.jsxs)(c.Menu,{"aria-label":s.formatMessage(b),children:[(0,t.jsx)(c.Separator,{}),i.map(e=>(0,t.jsx)(c.MenuItem,{label:e.label,icon:e.icon,href:e.href,as:e.as},e.label))]}):null;return a=e&&!r?.length?(0,t.jsx)(g.View,{grow:!0,justify:"center",align:"center",children:(0,t.jsx)(n.default,{})}):r?.length?(0,t.jsxs)(t.Fragment,{children:[(0,t.jsxs)(p.Text,{multiline:!1,variant:"small",color:"dimmer",clsx:(0,u.tw)("pb-100 pl-300"),children:["Recent ",d]}),(0,t.jsx)(c.Menu,{"aria-label":`Recent ${d}`,children:r.map(e=>(0,t.jsx)(h,{repl:e},e.id))})]}):(0,t.jsx)(g.View,{grow:!0,justify:"center",align:"center",children:(0,t.jsx)(p.Text,{variant:"small",color:"dimmer",clsx:(0,u.tw)("p-200 text-center"),children:s.formatMessage({id:"home.recentlyViewedEntities",defaultMessage:"Recently viewed {entityName} will show up here"},{entityName:d})})}),(0,t.jsxs)(g.View,{grow:!0,clsx:(0,u.tw)(u.tw.designSystemDeviation("min-w-[220px] max-w-[340px] min-h-[60px]")),children:[a,m]})},b={id:"home.globalSidebarLibrary",defaultMessage:"Library"};e.s(["OrgSidebarRecentRepls",0,({orgId:e,links:r})=>{let{data:i,loading:n}=(0,a.useSidebarRecentOrgReplsQuery)({variables:{count:6,orgId:e},ssr:!1,fetchPolicy:"cache-and-network",nextFetchPolicy:"cache-first"}),s=i?.currentUser?.org?.__typename==="Org"?i?.currentUser?.org?.recentRepls.items:void 0;return(0,t.jsx)(f,{loading:n,recentRepls:s,links:r})},"RECENT_REPLS_SIDEBAR_MENU_COUNT",0,6,"RecentReplsHoverCard",0,({children:e,content:a,isDisabled:n=!1,onOpenChange:o})=>{let l=(0,s.useIsMobile)(),u=(0,r.useRouter)(),c=(0,i.useRef)(!1),[p,g]=(0,i.useState)(0);return(0,i.useEffect)(()=>{if(!u)return;let e=()=>{c.current&&(c.current=!1,o?.(!1),g(e=>e+1))};return u.events.on("routeChangeStart",e),()=>{u.events.off("routeChangeStart",e)}},[o,u]),(0,t.jsx)(d.HoverCard,{isDisabled:l||n,openDelayMs:500,placement:"right top",offset:4,opensOnFocus:!1,content:a,onOpenChange:e=>{c.current=e,o?.(e)},children:e},p)},"SidebarRecentRepls",0,({links:e})=>{let{data:r,loading:i}=(0,a.useSidebarRecentReplsQuery)({variables:{count:6},ssr:!1,fetchPolicy:"cache-and-network",nextFetchPolicy:"cache-first"});return(0,t.jsx)(f,{loading:i,recentRepls:r?.allRecentRepls,links:e})}])},572810,e=>{e.v({radius:"SkipNav-module__6P6gMq__radius"})},668186,e=>{"use strict";var t=e.i(276385);e.i(214847);var r=e.i(20397),i=e.i(27923),a=e.i(406664),n=e.i(572810);e.s(["SkipNav",0,({children:e,contentId:s})=>{let o=(0,a.useCreateInteractive)({variant:"filled"});return(0,t.jsx)("a",{href:`#${s}`,clsx:[i.tw.merge("fixed top-0 left-100 z-5000 -translate-y-full p-100 opacity-0",i.tw.on("focus")("translate-y-0 opacity-100"),i.tw.external(o.clsx)),n.default.radius],style:o.style,children:e??(0,t.jsx)(r.FormattedMessage,{id:"home.skipToContent",defaultMessage:"Skip to content"})})}])},517414,e=>{"use strict";var t=e.i(351623),r=e.i(317349),i=e.i(748538),a=e.i(781258),n=e.i(394701),s=e.i(80593),o=e.i(299020);let l={},u=t.gql`
    fragment ComponentsReplActions on Repl {
  id
  url
  title
  slug
  user {
    id
    username
  }
  ...DeleteReplDialogRepl
  ...EditReplFormRepl
  ...TransferReplToOrgDialogRepl
  ...TransferReplBetweenWorkspacesDialogRepl
  ...LeaveMultiplayerReplDialogRepl
  owner {
    __typename
    ... on Team {
      id
    }
    ... on User {
      id
    }
  }
  org {
    id
  }
  isCurrentUserStarred
  isStackTemplate
  authorizations {
    deleteRepl {
      isAuthorized
    }
    editFolder {
      isAuthorized
    }
    fork {
      isAuthorized
    }
    removeSelf {
      isAuthorized
    }
    star {
      isAuthorized
    }
  }
}
    ${r.DeleteReplDialogReplFragmentDoc}
${i.EditReplFormReplFragmentDoc}
${a.TransferReplToOrgDialogReplFragmentDoc}
${n.TransferReplBetweenWorkspacesDialogReplFragmentDoc}
${s.LeaveMultiplayerReplDialogReplFragmentDoc}`,d=t.gql`
    mutation ReplActionsToggleReplPin($input: ToggleReplPinInput!) {
  toggleReplPin(input: $input) {
    ... on Repl {
      id
      isCurrentUserStarred
    }
    ... on Error {
      message
    }
  }
}
    `,c=t.gql`
    mutation AddOrgStackTemplate($orgId: String!, $replId: String!, $order: Float) {
  addOrgStackTemplate(orgId: $orgId, replId: $replId, order: $order) {
    success
    message
    repl {
      id
      isStackTemplate
    }
  }
}
    `,p=t.gql`
    mutation RemoveOrgStackTemplate($orgId: String!, $replId: String!) {
  removeOrgStackTemplate(orgId: $orgId, replId: $replId) {
    success
    message
    repl {
      id
      isStackTemplate
    }
  }
}
    `,g=t.gql`
    mutation ReplActionsMoveToFolder($replIds: [String!]!, $folderIds: [String!]!, $destFolderId: String!) {
  moveItemsToFolder(
    replIds: $replIds
    folderIds: $folderIds
    destFolderId: $destFolderId
  ) {
    ... on Repl {
      __typename
      id
    }
  }
}
    `;e.s(["ComponentsReplActionsFragmentDoc",0,u,"useAddOrgStackTemplateMutation",0,function(e){let t={...l,...e};return o.useMutation(c,t)},"useRemoveOrgStackTemplateMutation",0,function(e){let t={...l,...e};return o.useMutation(p,t)},"useReplActionsMoveToFolderMutation",0,function(e){let t={...l,...e};return o.useMutation(g,t)},"useReplActionsToggleReplPinMutation",0,function(e){let t={...l,...e};return o.useMutation(d,t)}])},566317,e=>{"use strict";var t=e.i(351623),r=e.i(566977),i=e.i(989074);let a=t.gql`
    fragment ReplsTableRepl on Repl {
  ...ReplCardRepl
  url
  timeCreated
  hostingDeployment {
    __typename
    ... on HostingDeployment {
      ...BuildStatusBadgeHostingDeployment
      currentBuild {
        id
        isPrivate
      }
    }
  }
}
    ${r.ReplCardReplFragmentDoc}
${i.BuildStatusBadgeHostingDeploymentFragmentDoc}`;e.s(["ReplsTableReplFragmentDoc",0,a])},806689,e=>{"use strict";var t=e.i(351623),r=e.i(566317);let i=t.gql`
    fragment ReplsGridRepl on Repl {
  id
  ...ReplsTableRepl
}
    ${r.ReplsTableReplFragmentDoc}`;e.s(["ReplsGridReplFragmentDoc",0,i])},882703,e=>{"use strict";var t=e.i(351623),r=e.i(566317),i=e.i(806689);let a=t.gql`
    fragment ReplsViewRepl on Repl {
  id
  ...ReplsTableRepl
  ...ReplsGridRepl
}
    ${r.ReplsTableReplFragmentDoc}
${i.ReplsGridReplFragmentDoc}`;e.s(["ReplsViewReplFragmentDoc",0,a])},273388,e=>{"use strict";var t=e.i(351623),r=e.i(882703),i=e.i(566977),a=e.i(344480),n=e.i(975473);let s={},o=t.gql`
    query OrgReplsV2($orgId: String!, $input: ReplsInput!) {
  currentUser {
    id
  }
  getOrg(orgId: $orgId) {
    __typename
    ... on Org {
      id
      replsV2(input: $input) {
        __typename
        ... on ReplConnection {
          items {
            ...ReplsViewRepl
            ...ReplCardOrgRepl
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
    ... on Error {
      message
    }
  }
}
    ${r.ReplsViewReplFragmentDoc}
${i.ReplCardOrgReplFragmentDoc}`;e.s(["useOrgReplsV2LazyQuery",0,function(e){let t={...s,...e};return n.useLazyQuery(o,t)},"useOrgReplsV2Query",0,function(e){let t={...s,...e};return a.useQuery(o,t)}])},297138,e=>{"use strict";var t=e.i(389959),r=e.i(908796),i=e.i(273388),a=e.i(320216);e.i(214847);var n=e.i(864300),s=e.i(738720);function o({publishedOnly:e,sort:t,cursor:r,search:i,filters:a}){let n=(0,s.buildSharedFilters)({filters:a,publishedOnly:e});return{count:s.PAGE_SIZE,sort:t,cursor:r,...i?{search:i}:{},...Object.keys(n).length>0?{filters:n}:{}}}e.s(["buildOrgInput",0,o,"useOrgReplsData",0,function({orgId:e,publishedOnly:l,sortType:u,sortDirection:d,debouncedSearch:c,filters:p}){let{showError:g}=(0,a.default)(),m=(0,n.useIntl)(),h=r.CurrentUserReplsSortTypeEnum.LastOpened,f=o({publishedOnly:l,sort:(0,s.buildSort)({sortType:u,sortDirection:d,defaultSortType:h}),cursor:void 0,search:c||void 0,filters:p}),{data:b,previousData:y,loading:S,error:v,fetchMore:_}=(0,i.useOrgReplsV2Query)({refetchWritePolicy:"overwrite",fetchPolicy:"cache-and-network",nextFetchPolicy:"cache-first",notifyOnNetworkStatusChange:!0,ssr:!1,variables:{orgId:e,input:f}}),R=b?.getOrg?.__typename==="Org"?b.getOrg:void 0,C=R?.replsV2?.__typename==="ReplConnection"?R.replsV2:void 0,T=b?.getOrg&&"Org"!==b.getOrg.__typename&&"message"in b.getOrg||R?.replsV2?.__typename==="UserError",{search:x,...I}=f,A=JSON.stringify({orgId:e,...I}),O=(0,t.useRef)(null);T||v?O.current=null:C&&(O.current={nonSearchInputsKey:A,connection:C});let P=(0,t.useRef)(c),k=(0,t.useRef)(!1);P.current!==c&&(P.current=c,k.current=!0),S||(k.current=!1);let M=S&&""!==c&&k.current,w=S&&O.current?.nonSearchInputsKey===A?O.current?.connection:void 0,E=C??w,j=E?.items??s.EMPTY_ITEMS,D=b?.currentUser?.id??y?.currentUser?.id,L=E?.pageInfo.hasNextPage??!1,F=v?.message,G=b?.getOrg&&"Org"!==b.getOrg.__typename&&"message"in b.getOrg?b.getOrg.message:void 0,U=R?.replsV2?.__typename==="UserError"?R.replsV2.message:void 0,z=(0,t.useCallback)(async()=>{if(S||!E?.pageInfo.nextCursor)return;let t=o({publishedOnly:l,sort:(0,s.buildSort)({sortType:u,sortDirection:d,defaultSortType:h}),cursor:E.pageInfo.nextCursor,search:c||void 0,filters:p});await _({variables:{orgId:e,input:t},updateQuery:(e,{fetchMoreResult:t})=>{let r=t.getOrg?.__typename==="Org"?t.getOrg:void 0;if(!r||"ReplConnection"!==r.replsV2.__typename)return g(m.formatMessage({id:"home.couldNotLoadMore",defaultMessage:"Could not load more results - please refresh the page and try again"})),e;let i=e.getOrg?.__typename==="Org"?e.getOrg:void 0,a=[...i?.replsV2.__typename==="ReplConnection"?i.replsV2.items:[],...r.replsV2.items];return{...t,getOrg:{...r,replsV2:{...r.replsV2,items:a}}}}})},[S,_,e,l,u,d,h,c,p,E?.pageInfo.nextCursor,g,m]);return{items:j,loading:S,isSearchLoading:M,canLoadMore:L,onLoadMore:z,errorMessage:F??G??U,currentUserId:D}},"useOrgReplsLazyQuery",0,function(){return(0,i.useOrgReplsV2LazyQuery)()}])},483460,e=>{"use strict";var t=e.i(351623),r=e.i(882703),i=e.i(344480),a=e.i(975473);let n={},s=t.gql`
    query CurrentUserRepls($input: CurrentUserReplsInput!) {
  currentUser {
    id
    repls(input: $input) {
      __typename
      ... on ReplConnection {
        items {
          ...ReplsViewRepl
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
    ${r.ReplsViewReplFragmentDoc}`;e.s(["useCurrentUserReplsLazyQuery",0,function(e){let t={...n,...e};return a.useLazyQuery(s,t)},"useCurrentUserReplsQuery",0,function(e){let t={...n,...e};return i.useQuery(s,t)}])},818373,e=>{"use strict";var t=e.i(389959),r=e.i(908796),i=e.i(483460),a=e.i(320216);e.i(214847);var n=e.i(864300),s=e.i(738720);function o({publishedOnly:e,sort:t,cursor:r,search:i,filters:a}){let n=(0,s.buildSharedFilters)({filters:a,publishedOnly:e});return{count:s.PAGE_SIZE,sort:t,cursor:r,...i?{search:i}:{},...Object.keys(n).length>0?{filters:n}:{}}}e.s(["buildPersonalInput",0,o,"usePersonalReplsData",0,function({publishedOnly:e,sortType:l,sortDirection:u,debouncedSearch:d,filters:c}){let{showError:p}=(0,a.default)(),g=(0,n.useIntl)(),m=r.CurrentUserReplsSortTypeEnum.LastOpened,h=o({publishedOnly:e,sort:(0,s.buildSort)({sortType:l,sortDirection:u,defaultSortType:m}),cursor:void 0,search:d||void 0,filters:c}),{data:f,previousData:b,loading:y,error:S,fetchMore:v}=(0,i.useCurrentUserReplsQuery)({refetchWritePolicy:"overwrite",fetchPolicy:"cache-and-network",nextFetchPolicy:"cache-first",notifyOnNetworkStatusChange:!0,ssr:!1,variables:{input:h}}),_=f?.currentUser?.repls?.__typename==="ReplConnection"?f.currentUser.repls:void 0,R=f?.currentUser?.repls?.__typename==="NotFoundError"||f?.currentUser?.repls?.__typename==="UserError",{search:C,...T}=h,x=JSON.stringify(T),I=(0,t.useRef)(null);R||S?I.current=null:_&&(I.current={nonSearchInputsKey:x,connection:_});let A=(0,t.useRef)(d),O=(0,t.useRef)(!1);A.current!==d&&(A.current=d,O.current=!0),y||(O.current=!1);let P=y&&""!==d&&O.current,k=y&&I.current?.nonSearchInputsKey===x?I.current?.connection:void 0,M=_??k,w=M?.items??s.EMPTY_ITEMS,E=f?.currentUser?.id??b?.currentUser?.id,j=M?.pageInfo.hasNextPage??!1,D=S?.message,L=f?.currentUser?.repls?.__typename==="NotFoundError"||f?.currentUser?.repls?.__typename==="UserError"?f.currentUser.repls.message:void 0,F=(0,t.useCallback)(async()=>{if(y||!M?.pageInfo.nextCursor)return;let t=o({publishedOnly:e,sort:(0,s.buildSort)({sortType:l,sortDirection:u,defaultSortType:m}),cursor:M.pageInfo.nextCursor,search:d||void 0,filters:c});await v({variables:{input:t},updateQuery:(e,{fetchMoreResult:t})=>{if(t.currentUser?.repls?.__typename!=="ReplConnection")return p(g.formatMessage({id:"home.couldNotLoadMore",defaultMessage:"Could not load more results - please refresh the page and try again"})),e;let r=[...e.currentUser?.repls?.__typename==="ReplConnection"?e.currentUser.repls.items:[],...t.currentUser.repls.items];return{...t,currentUser:{...t.currentUser,repls:{...t.currentUser.repls,items:r}}}}})},[y,v,e,l,u,m,d,c,M?.pageInfo.nextCursor,p,g]);return{items:w,loading:y,isSearchLoading:P,canLoadMore:j,onLoadMore:F,errorMessage:D??L,currentUserId:E}},"usePersonalReplsLazyQuery",0,function(){return(0,i.useCurrentUserReplsLazyQuery)()}])},155606,e=>{"use strict";var t=e.i(389959),r=e.i(908796),i=e.i(297138),a=e.i(818373),n=e.i(738720);e.s(["usePrewarmRepls",0,function({orgId:e}){let s=(0,t.useRef)(!1),[o]=(0,a.usePersonalReplsLazyQuery)(),[l]=(0,i.useOrgReplsLazyQuery)();return(0,t.useCallback)(()=>{if(s.current)return;s.current=!0;let t=(0,n.buildSort)({sortType:void 0,sortDirection:void 0,defaultSortType:r.CurrentUserReplsSortTypeEnum.LastOpened});e?l({variables:{orgId:e,input:(0,i.buildOrgInput)({publishedOnly:!1,sort:t,cursor:void 0,search:void 0,filters:[]})}}):o({variables:{input:(0,a.buildPersonalInput)({publishedOnly:!1,sort:t,cursor:void 0,search:void 0,filters:[]})}})},[e,l,o])}])},738720,e=>{"use strict";var t=e.i(389959),r=e.i(908833);e.i(81282);var i=e.i(750185);e.i(257697);var a=e.i(312806),n=e.i(223481),s=e.i(908796),o=e.i(569910),l=e.i(776065);let u=i.Type.Union([i.Type.Literal(s.CurrentUserReplsSortTypeEnum.CreationDate),i.Type.Literal(s.CurrentUserReplsSortTypeEnum.LastOpened),i.Type.Literal(s.CurrentUserReplsSortTypeEnum.LastUpdated),i.Type.Literal(s.CurrentUserReplsSortTypeEnum.PublishedAt)]),d=i.Type.Union([i.Type.Literal(s.CurrentUserReplsSortDirectionEnum.Ascending),i.Type.Literal(s.CurrentUserReplsSortDirectionEnum.Descending)]),c=i.Type.Union([i.Type.Literal(s.CurrentUserReplsDeploymentStatusEnum.Deployed),i.Type.Literal(s.CurrentUserReplsDeploymentStatusEnum.Failed),i.Type.Literal(s.CurrentUserReplsDeploymentStatusEnum.NotDeployed),i.Type.Literal(s.CurrentUserReplsDeploymentStatusEnum.Success)]),p=i.Type.Union([i.Type.Literal("grid"),i.Type.Literal("table")]),g=i.Type.Union([i.Type.Literal("__MULTIPLAYER_REPLS__"),i.Type.String({pattern:"^__TEAM__\\d+__$"})]),m={lastUpdated:s.CurrentUserReplsSortTypeEnum.LastOpened,createdAt:s.CurrentUserReplsSortTypeEnum.CreationDate};e.s(["COLUMN_TO_SORT_TYPE",0,m,"EMPTY_ITEMS",0,[],"PAGE_SIZE",0,24,"REPLS_REFETCH_QUERIES",0,["CurrentUserRepls","OrgReplsV2","ReplsList"],"buildFiltersFromQueryParams",0,function({availableFilters:e,statusFilter:t,selectedCreators:r,folderFilter:i,buildTypeFilter:a}){return(e.includes("buildType")?e:[...e,"buildType"]).map(e=>{switch(e){case"publishedStatus":return{type:"publishedStatus",value:t};case"creator":return{type:"creator",value:r};case"folder":return{type:"folder",value:i};case"buildType":return{type:"buildType",value:a}}})},"buildSharedFilters",0,function({filters:e,publishedOnly:t}){let r={},i=!1;for(let t of e)switch(t.type){case"publishedStatus":t.value&&(r.deploymentStatus=t.value,i=!0);break;case"creator":t.value.size>0&&(r.createdBy={userIds:Array.from(t.value)});break;case"folder":t.value&&(r.folderId=t.value);break;case"buildType":t.value&&(r.artifactKind=t.value);break;default:(0,o.default)(t)}return!i&&t&&(r.deploymentStatus=s.CurrentUserReplsDeploymentStatusEnum.Deployed),r},"buildSort",0,function({sortType:e,sortDirection:t,defaultSortType:r=s.CurrentUserReplsSortTypeEnum.LastUpdated}){return{sortType:e??r,direction:t??s.CurrentUserReplsSortDirectionEnum.Descending,pinnedFirst:!0}},"useReplsQueryParams",0,function(){let e=(0,l.useQueryParam)("sort","string"),i=(0,l.useQueryParam)("order","string"),s=(0,l.useQueryParam)("search","string"),o=(0,l.useQueryParam)("status","string"),m=(0,l.useQueryParam)("creator","string"),h=(0,l.useQueryParam)("view","string"),f=(0,l.useQueryParam)("folder","string"),b=(0,l.useQueryParam)("buildType","string");return(0,t.useMemo)(()=>{let t=a.Value.Check(u,e)?e:void 0,l=a.Value.Check(d,i)?i:void 0,y=a.Value.Check(c,o)?o:void 0,S=new Set;m&&m.split(",").filter(Boolean).map(Number).filter(e=>!Number.isNaN(e)&&e>0).forEach(e=>S.add(e));let v=a.Value.Check(p,h)?h:void 0;return{sortType:t,sortDirection:l,search:s,statusFilter:y,selectedCreators:S,view:v,folderFilter:f&&((0,n.validate)(f)||a.Value.Check(g,f))?f:void 0,buildTypeFilter:a.Value.Check(r.ArtifactSchema.properties.kind,b)?b:void 0}},[e,i,s,o,m,h,f,b])}])},485512,e=>{e.v({root:"ResizeHandle-module__IGYIEq__root",sidebarHandle:"ResizeHandle-module__IGYIEq__sidebarHandle",sidebarHandleInsetEnds:"ResizeHandle-module__IGYIEq__sidebarHandleInsetEnds"})},374041,e=>{"use strict";var t=e.i(276385),r=e.i(389959),i=e.i(14976),a=e.i(438932),n=e.i(89148),s=e.i(61732),o=e.i(485512);let l=(0,n.cvarsFrom)("ResizeHandle.module.css",["--width","--height","--transform-x","--transform-y","--z-index","--cursor"]),u=(0,r.memo)(function({rect:e,onResize:n,onDoubleClick:u,insetEnds:d=!1,appearance:c="default",idleZIndex:p,activeZIndex:g,children:m}){let h=(0,r.useRef)(null),f=(0,r.useRef)(null),b=(0,r.useRef)(!1);return(0,a.useStyleSet)(h,e,({x:e,y:t,width:r,height:i})=>({[l.width]:r+"px",[l.height]:i+"px",[l.transformX]:Math.round(e)+"px",[l.transformY]:Math.round(t)+"px",[l.zIndex]:(b.current?g:p).toString(),[l.cursor]:"col-resize",display:0===r?"none":""}),[g,p]),(0,t.jsx)(s.View,{innerRef:h,clsx:[o.default.root,o.default.sidebarHandle,{[o.default.sidebarHandleInsetEnds]:d}],"data-appearance":c,onPointerDown:e=>{e.defaultPrevented||(0,i.measureMouseMove)(e,{start(){f.current=document.activeElement,b.current=!0,h.current?.style.setProperty(l.zIndex,g.toString()),n.start()},move({delta:e}){n.update(e)},end(){b.current=!1,h.current?.style.setProperty(l.zIndex,p.toString()),n.end(),f.current?.focus?.(),f.current=null}})},onDoubleClick:u?e=>{e.defaultPrevented||u()}:void 0,children:m})});e.s(["SidebarResizeHandle",0,u,"resizeHandleCvars",0,l])},559857,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let i={},a=t.gql`
    query FabricOnlyAppCreationPolicy($orgId: String!) {
  getOrg(orgId: $orgId) {
    ... on Org {
      id
      authorizations {
        createNonFabricApp {
          isAuthorized
        }
      }
    }
  }
}
    `;e.s(["useFabricOnlyAppCreationPolicyQuery",0,function(e){let t={...i,...e};return r.useQuery(a,t)}])},183119,e=>{"use strict";var t=e.i(559857);e.s(["useFabricOnlyAppCreation",0,function(e){let r=!e,{data:i,loading:a,error:n}=(0,t.useFabricOnlyAppCreationPolicyQuery)({variables:{orgId:e??""},ssr:!1,skip:r,fetchPolicy:"cache-and-network",nextFetchPolicy:"cache-first"});if(r)return{isLocked:!1,isResolving:!1,isError:!1};if(n)return{isLocked:!1,isResolving:!1,isError:!0};let s=i?.getOrg.__typename==="Org"?i.getOrg:null;return s?{isLocked:function({canCreateNonFabricApp:e}){return!e}({canCreateNonFabricApp:s.authorizations.createNonFabricApp.isAuthorized}),isResolving:!1,isError:!1}:a?{isLocked:!1,isResolving:!0,isError:!1}:{isLocked:!1,isResolving:!1,isError:!0}}])},793344,e=>{"use strict";var t=e.i(351623);e.i(344480);var r=e.i(975473);let i={},a=t.gql`
    fragment EmptyImportNothingTemplateRepl on Repl {
  id
  deployment {
    id
    activeRelease {
      id
    }
  }
}
    `,n=t.gql`
    query EmptyImportNothingTemplate {
  repl(url: "/@replit/Nothing") {
    __typename
    ... on Repl {
      ...EmptyImportNothingTemplateRepl
    }
    ... on ReplRedirect {
      repl {
        ...EmptyImportNothingTemplateRepl
      }
    }
  }
}
    ${a}`;e.s(["useEmptyImportNothingTemplateLazyQuery",0,function(e){let t={...i,...e};return r.useLazyQuery(n,t)}])},798333,e=>{"use strict";var t=e.i(389959),r=e.i(793344),i=e.i(636310),a=e.i(327768),n=e.i(738522),s=e.i(320216);e.i(214847);var o=e.i(864300);e.s(["useCreateEmptyRepl",0,function({orgId:e,trackingData:l}){let u=(0,o.useIntl)(),{showError:d}=(0,s.default)(),[c,p]=(0,t.useState)(!1),g=(0,t.useCallback)(()=>p(!1),[]),m=(0,i.useCarryHomeAgentConfig)(e),h=(0,t.useCallback)(async e=>{e?.createRepl.__typename==="Repl"&&await m(e.createRepl.id)},[m]),[f]=(0,n.default)({onFork:h,onError:g,onNavigationComplete:g}),[b]=(0,r.useEmptyImportNothingTemplateLazyQuery)(),y=(0,a.useLazyGeneratedReplTitle)();return{createEmptyRepl:(0,t.useCallback)(async()=>{if(c)return;p(!0);let[{data:t},r]=await Promise.all([b(),y()]),i=t?"Repl"===t.repl.__typename?t.repl:"ReplRedirect"===t.repl.__typename?t.repl.repl??null:null:null;if(!i){p(!1),d(u.formatMessage({id:"createRepl.emptyProjectLoadSourceError",defaultMessage:"Could not load empty project source. Refresh and try again."}));return}f({originId:i.id,replReleaseId:i.deployment?.activeRelease?.id||void 0,title:r,isTitleAutoGenerated:!0,isPrivate:!0,orgId:e,forkToPersonal:null==e,zealot:!0,trackingData:l})},[f,u,c,y,b,e,p,d,l]),isStarting:c}}])},636310,e=>{"use strict";var t=e.i(389959),r=e.i(11990),i=e.i(447963),a=e.i(394970),n=e.i(18109),s=e.i(215515);e.s(["useCarryHomeAgentConfig",0,function(e){let o=(0,n.useCarryAgentConfigToRepl)(),l=(0,s.useLazyConversationAgentAuthorizations)(e);return(0,t.useCallback)(async t=>{var n;let s,u,d=await l();if((0,a.areAccountAgentSettingsLocked)(e,d.paidAgentAuthorizationCode))return;let c=(n=d.isIntelligentAutoModeAuthorized,s=(0,i.readStoredAutoMode)(e)??(n||void 0),"UNSPECIFIED"===(u=(0,i.modelProfileForTier)((0,i.readStoredTier)("home",e)))&&void 0===s?null:{modelProfile:u,modelPicks:s?{powerModel:r.INTELLIGENT_AUTO_MODEL_SLUG}:{},intelligentAutoMode:s});null!==c&&await o(t,c)},[o,l,e])}])},773185,e=>{"use strict";var t=e.i(389959),r=e.i(742881),i=e.i(636310),a=e.i(738522),n=e.i(320216);e.i(214847);var s=e.i(864300),o=e.i(933302);e.s(["useCreateBlankDesignRepl",0,function({orgId:e,trackingData:l}){let u=(0,s.useIntl)(),{showError:d}=(0,n.default)(),c=(0,o.useDynamicConfigParam)("design_blank_source_repl","repl_id",""),[p,g]=(0,t.useState)(!1),m=(0,i.useCarryHomeAgentConfig)(e),h=(0,t.useCallback)(async e=>{let t=e?.createRepl.__typename==="Repl"?e.createRepl:null;t&&((0,r.markRemixGalleryAutostart)(t.id,{autostart:!1,canvasMode:!0}),await m(t.id))},[m]),f=(0,t.useCallback)(()=>g(!1),[]),[b]=(0,a.default)({onFork:h,onError:f,onNavigationComplete:f});return{createBlankDesignRepl:(0,t.useCallback)(()=>{if(!p){if(!c)return void d(u.formatMessage({id:"home.designBannerSourceUnavailable",defaultMessage:"Creating a blank Design is not available right now. Try again later."}));g(!0),b({originId:c,title:u.formatMessage({id:"home.designBannerProjectTitle",defaultMessage:"Replit Design Project"}),isPrivate:!0,orgId:e,forkToPersonal:null==e,zealot:!0,isWebDesignMockup:!0,isTitleAutoGenerated:!1,trackingData:l})}},[b,u,p,e,g,d,c,l]),isSourceAvailable:!!c,isStarting:p}}])},729593,e=>{"use strict";var t=e.i(351623),r=e.i(344480),i=e.i(975473);let a={},n=t.gql`
    query CreateReplTitle($teamId: Int, $title: String) {
  replTitle(teamId: $teamId, title: $title)
}
    `;e.s(["useCreateReplTitleLazyQuery",0,function(e){let t={...a,...e};return i.useLazyQuery(n,t)},"useCreateReplTitleQuery",0,function(e){let t={...a,...e};return r.useQuery(n,t)}])},327768,e=>{"use strict";var t=e.i(389959),r=e.i(729593);e.s(["maxTitleLength",0,60,"useLazyGeneratedReplTitle",0,function(){let[e]=(0,r.useCreateReplTitleLazyQuery)({fetchPolicy:"network-only"});return(0,t.useCallback)(async()=>(await e()).data?.replTitle||void 0,[e])}])},989074,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment BuildStatusBadgeHostingDeployment on HostingDeployment {
  id
  currentBuild {
    id
    status
    timeCreated
    provider
    user {
      id
      displayName
    }
  }
}
    `;e.s(["BuildStatusBadgeHostingDeploymentFragmentDoc",0,r])},113212,e=>{"use strict";var t=e.i(351623),r=e.i(989074),i=e.i(323604);let a=t.gql`
    fragment ReplCardArtifact on ReplArtifact {
  artifactId
  title
  kind
  previewPath
  latestScreenshotUri
  latestScreenshotUrl
}
    `;r.BuildStatusBadgeHostingDeploymentFragmentDoc,i.DeploymentLinkFragmentDoc,e.s(["ReplCardArtifactFragmentDoc",0,a])},607757,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let i={},a=t.gql`
    query HomeArrivalTourEntryPoint {
  currentUser {
    id
    hasUserCompletedOnboarding
    customer {
      id
      authorizations {
        homeArrivalTourCohort
      }
    }
    toursSeen(
      tours: ["agentic-home-arrival-tour-new-starter-2026-08", "agentic-home-arrival-tour-new-core-or-pro-2026-08", "agentic-home-arrival-tour-returning-starter-2026-08", "agentic-home-arrival-tour-returning-core-or-pro-2026-08", "agentic-home-arrival-tour-returning-enterprise-2026-08"]
    ) {
      id
      seen
    }
  }
}
    `;e.s(["useHomeArrivalTourEntryPointQuery",0,function(e){let t={...i,...e};return r.useQuery(a,t)}])},135822,e=>{e.v({mobileEntry:"HomeArrivalTourEntryPoint-module__2r0txW__mobileEntry"})},476384,e=>{"use strict";var t=e.i(276385),r=e.i(196786),i=e.i(15801),a=e.i(389959),n=e.i(607757),s=e.i(965097);e.i(214847);var o=e.i(864300),l=e.i(753451),u=e.i(415541),d=e.i(709485),c=e.i(242917),p=e.i(92120),g=e.i(785051),m=e.i(126942),h=e.i(643484),f=e.i(488299),b=e.i(61732),y=e.i(933302),S=e.i(135822);let v=(0,r.default)(()=>e.A(652450).then(e=>e.HomeArrivalTourTips),{loadableGenerated:{modules:[627940]},ssr:!1});e.s(["HomeArrivalTourEntryPoint",0,function({presentation:e}){let r=(0,o.useIntl)(),{isVisible:_,segment:R,tipIds:C,reopen:T,isReplayingTips:x,finishTipsReplay:I}=function(){let e=(0,i.useRouter)(),{show:t}=(0,c.useGlobalModal)(),r="/t/[orgSlug]"===e.pathname,s=(0,y.useFeatureGate)("gate_agentic_home_arrival_tour",!1),o=(0,y.useFeatureGate)("gate_agentic_home_arrival_tour_modal",!1),h=(0,y.useFeatureGate)("gate_agentic_home_arrival_tour_enterprise",!1),f="/home"===e.pathname,b=(0,l.useIsInBonsaiWebview)(),{data:S}=(0,n.useHomeArrivalTourEntryPointQuery)({skip:!s||b||!f&&!r}),v=(0,g.findSeenHomeArrivalTourSegment)(S?.currentUser?.toursSeen??[]),_=(0,g.getHomeArrivalTourSegment)({hasCompletedOnboarding:S?.currentUser?.hasUserCompletedOnboarding,sawIncompleteOnboardingThisSession:(0,p.getSawIncompleteOnboarding)(S?.currentUser?.id),cohort:S?.currentUser?.customer.authorizations.homeArrivalTourCohort??null}),R=v??_,C=null===R?[]:(0,m.getHomeArrivalTipIds)(R,o),T=null!==R&&(0,g.isHomeArrivalTourSegmentOnSurface)(R,r?"org":"personal"),x="returning_enterprise"!==R||h,I="returning_enterprise"===R||o,A=T&&x&&I&&("returning_enterprise"!==R||null!==v)&&C.length>0&&s&&!b,[O,P]=(0,a.useState)(!1),k=(0,a.useCallback)(()=>P(!1),[]),M=e.asPath.split("?")[0];return(0,a.useEffect)(()=>{P(!1)},[M]),{isVisible:A,segment:R,tipIds:C,reopen:()=>{if(null!==R){if("returning_enterprise"===R)return void P(!0);(0,u.track)(d.events.MODAL_VIEWED,{unsolicited:!1,modalName:"home_arrival_tour_global"}),t("HomeArrivalTourModal",{mode:"reopen"})}},isReplayingTips:O,finishTipsReplay:k}}();if(!_)return null;let A=x&&null!==R?(0,t.jsx)(v,{tipIds:C,onFinish:I,tourType:R,trigger:"manual_replay"}):null,O="returning_enterprise"===R?r.formatMessage({id:"home.arrivalTourEntryRecentUpdates",defaultMessage:"Recent updates"}):r.formatMessage({id:"home.arrivalTourEntryLearnMore",defaultMessage:"Learn more"});return"icon"===e?(0,t.jsxs)(b.View,{clsx:S.default.mobileEntry,children:[(0,t.jsx)(f.IconButton,{variant:"nofill",onClick:T,"data-analytics-id":"home_arrival_tour_reopen_button",alt:O,children:(0,t.jsx)(s.default,{})}),A]}):(0,t.jsxs)(b.View,{px:6,pb:4,shrink:0,children:[(0,t.jsx)(h.Button,{variant:"nofill",alignment:"start",stretch:!0,onClick:T,"data-analytics-id":"home_arrival_tour_reopen_button",iconLeft:(0,t.jsx)(s.default,{}),text:O}),A]})}])},126942,e=>{"use strict";let t={new_starter:["first-message"],new_core_or_pro:["free-mode-paid"],returning_starter:["start-conversation","free-mode-starter","recents-pin"],returning_core_or_pro:["start-conversation","free-mode-paid","renamed-modes","recents-pin"],returning_enterprise:["start-conversation"]},r={new_starter:[],new_core_or_pro:[],returning_starter:["free-mode-starter"],returning_core_or_pro:["free-mode-paid"],returning_enterprise:[]},i=new Set(["free-mode-starter","free-mode-paid"]);e.s(["getHomeArrivalReplayTipIds",0,function(e,i){return i?r[e]:t[e]},"getHomeArrivalTipIds",0,function(e,r){let a=t[e];return r?a:a.filter(e=>!i.has(e))}])},566977,e=>{"use strict";var t=e.i(351623),r=e.i(319801),i=e.i(517414),a=e.i(113212),n=e.i(676107);let s=t.gql`
    fragment ReplCardRepl on Repl {
  id
  title
  iconUrl
  isPrivate
  isCurrentUserStarred
  timeUpdated
  lastOpened
  latestAgentScreenshotUrl
  ...ReplLinkRepl
  ...ComponentsReplActions
  artifacts {
    ...ReplCardArtifact
  }
  user {
    id
    username
    fullName
    image
  }
  hostingDeployment {
    __typename
    ... on HostingDeployment {
      id
      ...DeploymentItem
      latestBuildStatus
      currentBuild {
        id
        status
        timeCreated
        artifacts {
          ...HostingBuildArtifactFields
        }
        user {
          id
          displayName
        }
      }
    }
  }
  authorizations {
    star {
      isAuthorized
    }
    viewFileContents {
      isAuthorized
    }
    editFileContents {
      isAuthorized
      code
      message
    }
  }
}
    ${r.ReplLinkReplFragmentDoc}
${i.ComponentsReplActionsFragmentDoc}
${a.ReplCardArtifactFragmentDoc}
${n.DeploymentItemFragmentDoc}
${n.HostingBuildArtifactFieldsFragmentDoc}`,o=t.gql`
    fragment ReplCardOrgRepl on Repl {
  id
  config {
    isAgentStack
  }
  deploymentMetadata {
    ... on DeploymentMetadata {
      id
      url
    }
  }
}
    `;e.s(["ReplCardOrgReplFragmentDoc",0,o,"ReplCardReplFragmentDoc",0,s])},288249,e=>{"use strict";var t=e.i(351623),r=e.i(846545);let i={},a=t.gql`
    subscription CurrentUserReplAgentStatuses {
  currentUserReplAgentStatuses {
    replId
    statusV2
    label
    updatedAt
    appImageUrl
  }
}
    `;e.s(["useCurrentUserReplAgentStatusesSubscription",0,function(e){let t={...i,...e};return r.useSubscription(a,t)}])},687693,e=>{"use strict";var t=e.i(351623),r=e.i(846545);let i={},a=t.gql`
    subscription ReplAgentStates($authorizationEpoch: Int!, $replIds: [String!]!) {
  replAgentStates(authorizationEpoch: $authorizationEpoch, replIds: $replIds) {
    snapshot
    updates {
      replId
      status
      workingUntil
      lastAgentActivityAt
    }
  }
}
    `;e.s(["useReplAgentStatesSubscription",0,function(e){let t={...i,...e};return r.useSubscription(a,t)}])},505294,e=>{"use strict";var t=e.i(389959),r=e.i(908796),i=e.i(687693);e.i(242933);var a=e.i(279606);e.i(925218);var n=e.i(112077);e.s(["useCurrentUserReplAgentStateSource",0,function(e,s){let o=(0,n.useCreateObservable)(new Map),[l,u]=(0,t.useState)(0),d=(0,t.useRef)(new Map);return(0,t.useEffect)(()=>{let e=d.current;return()=>{e.forEach(clearTimeout)}},[]),(0,t.useEffect)(()=>{let e=new Set(s);d.current.forEach((t,r)=>{e.has(r)||(clearTimeout(t),d.current.delete(r))}),o.update(t=>[...t.keys()].every(t=>e.has(t))?t:new Map([...t].filter(([t])=>e.has(t))))},[s,o]),(0,i.useReplAgentStatesSubscription)({skip:e,variables:{authorizationEpoch:l,replIds:[...s]},fetchPolicy:"no-cache",onComplete:()=>u(e=>e+1),onData:({data:{data:e}})=>{let t=e?.replAgentStates;if(t){if(t.snapshot){d.current.forEach(clearTimeout),d.current.clear(),o.set(new Map(t.updates.map(e=>[e.replId,{status:e.status,workingUntil:e.workingUntil??null,lastAgentActivityAt:e.lastAgentActivityAt??null}]))),t.updates.forEach(c);return}t.updates.forEach(c),o.update(e=>{let r=new Map(e);return t.updates.forEach(e=>{r.set(e.replId,{status:e.status,workingUntil:e.workingUntil??null,lastAgentActivityAt:e.lastAgentActivityAt??null})}),r})}}}),(0,t.useMemo)(()=>a.Observable.from(o),[o]);function c(e){let t=d.current.get(e.replId);if(t&&(clearTimeout(t),d.current.delete(e.replId)),e.status!==r.ReplAgentStatus.Working||!e.workingUntil)return;let i=Math.max(0,Date.parse(e.workingUntil)-Date.now());d.current.set(e.replId,setTimeout(()=>{o.update(t=>{let i=t.get(e.replId);if(!i||i.workingUntil!==e.workingUntil)return t;let a=new Map(t);return a.set(e.replId,{status:r.ReplAgentStatus.Idle,workingUntil:null,lastAgentActivityAt:i.lastAgentActivityAt}),a})},i))}}])},234504,e=>{"use strict";var t=e.i(276385),r=e.i(389959),i=e.i(288249);e.i(242933);var a=e.i(279606);e.i(925218);var n=e.i(112077),s=e.i(505294);let o=(0,r.createContext)(null),l=(0,r.createContext)(null),u=(0,r.createContext)(null),d=(0,r.createContext)(null),c=a.Observable.of(new Map),p=a.Observable.of(new Map);function g(e){let t=(0,n.useCreateObservable)(new Map);return(0,i.useCurrentUserReplAgentStatusesSubscription)({skip:e,fetchPolicy:"no-cache",onData:({data:{data:e}})=>{e?.currentUserReplAgentStatuses&&t.set(new Map(e.currentUserReplAgentStatuses.map(e=>[e.replId,{statusV2:e.statusV2,label:e.label,updatedAt:e.updatedAt,appImageUrl:e.appImageUrl}])))}}),(0,r.useMemo)(()=>a.Observable.from(t),[t])}function m(e){let t=(0,r.useRef)(new Map),[i,a]=(0,r.useState)([]),n=(0,s.useCurrentUserReplAgentStateSource)(!e||0===i.length,i),o=(0,r.useCallback)(()=>{a([...new Set([...t.current.values()].flat())])},[]),l=(0,r.useCallback)(e=>{let r=Symbol();return t.current.set(r,e),o(),()=>{t.current.delete(r),o()}},[o]);return(0,r.useMemo)(()=>({source:n,register:l}),[l,n])}e.s(["AgentStatusContext",0,o,"AgentStatusProvider",0,function({children:e,enabled:i=!0,replAgentStateEnabled:a=i}){let n=(0,r.useContext)(o),s=(0,r.useContext)(l),h=(0,r.useContext)(u),f=(0,r.useContext)(d),b=g(!i||null!==n||null!==h),y=m(a&&null===s&&null===f),S=f??y,v=a?s??S.source:p;return(0,t.jsx)(o.Provider,{value:i?n??h??b:c,children:(0,t.jsx)(d.Provider,{value:S,children:(0,t.jsx)(l.Provider,{value:v,children:e})})})},"AgentStatusSourceProvider",0,function({children:e,enabled:i=!0,replAgentStateEnabled:a=i}){let n=(0,r.useContext)(u),s=(0,r.useContext)(d),o=g(!i||null!==n),l=m(a&&null===s);return(0,t.jsx)(u.Provider,{value:n??(i?o:null),children:(0,t.jsx)(d.Provider,{value:s??(a?l:null),children:e})})},"default",0,function(){let e=(0,r.useContext)(o);if(null===e)throw Error("useCurrentUserAgentStatus must be used within an AgentStatusProvider");return e},"useCurrentUserReplAgentState",0,function(){let e=(0,r.useContext)(l);if(null===e)throw Error("useCurrentUserReplAgentState must be used within an AgentStatusProvider");return e},"useReplAgentStateDemand",0,function(e){let t=(0,r.useContext)(d);(0,r.useEffect)(()=>t?.register(e),[t,e])}])},812406,e=>{"use strict";var t=e.i(908796),r=e.i(293982),i=e.i(806930),a=e.i(572599),n=e.i(493166),s=e.i(277257),o=e.i(937360),l=e.i(483620),u=e.i(650032);let d=[{value:"documents",assetTypes:[t.OrgSearchAssetFileType.Pdf,t.OrgSearchAssetFileType.WordDocument],Icon:a.default,label:"Documents",labelId:"library.typeDocuments"},{value:"spreadsheets",assetTypes:[t.OrgSearchAssetFileType.Table],Icon:l.default,label:"Spreadsheets",labelId:"library.typeSpreadsheets"},{value:"slides",assetTypes:[t.OrgSearchAssetFileType.Slides],Icon:r.default,label:"Presentations",labelId:"library.typePresentations"},{value:"images",assetTypes:[t.OrgSearchAssetFileType.Image],Icon:n.default,label:"Images",labelId:"library.typeImages"},{value:"html",assetTypes:[t.OrgSearchAssetFileType.Html],Icon:i.default,label:"HTML",labelId:"library.typeHtml"},{value:"text",assetTypes:[t.OrgSearchAssetFileType.Text],Icon:u.default,label:"Text",labelId:"library.typeText"},{value:"video",assetTypes:[t.OrgSearchAssetFileType.Video],Icon:o.default,label:"Video",labelId:"library.typeVideo"},{value:"audio",assetTypes:[t.OrgSearchAssetFileType.Audio],Icon:s.default,label:"Audio",labelId:"library.typeAudio"},{value:"other",assetTypes:[t.OrgSearchAssetFileType.Unspecified],Icon:a.default,label:"Other",labelId:"library.typeOther"}];e.s(["LIBRARY_ASSET_TYPE_FILTERS",0,d,"assetTypesForLibraryFilter",0,function(e){return d.flatMap(t=>t.value===e?[...t.assetTypes]:[])}])},805610,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment LibrarySearchAsset on Asset {
  id
  logicalId
  displayName
  contentType
  sizeBytes
  timeUpdated
  thumbnailUrl
  source {
    __typename
    ... on Repl {
      id
      title
      user {
        id
      }
    }
    ... on Conversation {
      id
      conversationTitle: title
    }
  }
}
    `,i=t.gql`
    fragment LibrarySearchArtifact on Artifact {
  id
  artifactId
  artifactType
  displayName
  timeCreated
  timeUpdated
  repl {
    id
    title
    user {
      id
    }
  }
}
    `,a=t.gql`
    fragment LibrarySearchResult on OrgSearchResult {
  __typename
  ... on OrgSearchAssetResult {
    asset {
      ...LibrarySearchAsset
    }
  }
  ... on OrgSearchArtifactResult {
    artifact {
      ...LibrarySearchArtifact
    }
  }
}
    ${r}
${i}`;e.s(["LibrarySearchResultFragmentDoc",0,a])},987520,e=>{"use strict";var t=e.i(908796),r=e.i(313287);let i={[t.ArtifactType.Api]:"api",[t.ArtifactType.Automation]:"automation",[t.ArtifactType.Cli]:"cli",[t.ArtifactType.Custom]:"custom",[t.ArtifactType.DataApp]:"data-app",[t.ArtifactType.Design]:"design",[t.ArtifactType.DesignSystem]:"design-system",[t.ArtifactType.Game]:"game",[t.ArtifactType.Mobile]:"mobile",[t.ArtifactType.Slides]:"slides",[t.ArtifactType.Video]:"video",[t.ArtifactType.Vnc]:"vnc",[t.ArtifactType.Web]:"web"};e.s(["ARTIFACT_KIND_BY_TYPE",0,i,"FILE_TYPE_FILTER",0,"file","toLibraryItems",0,function(e){return e.flatMap(e=>{switch(e.__typename){case"OrgSearchArtifactResult":var t;return[{id:(t=e.artifact).id,title:t.displayName,sourceTitle:t.repl.title,origin:{kind:"repl",id:t.repl.id,creatorId:t.repl.user?.id},updatedAt:new Date(t.timeUpdated??t.timeCreated).toISOString(),thumbnailUrl:`/data/repls/${encodeURIComponent(t.repl.id)}/artifact_screenshot?artifactId=${encodeURIComponent(t.artifactId)}`,presentation:{type:"artifact",artifactKind:i[t.artifactType]}}];case"OrgSearchAssetResult":return[function(e){let{source:t}=e,i="Conversation"===t.__typename?{kind:"conversation",id:t.id,assetKey:e.logicalId}:{kind:"repl",id:t.id,creatorId:t.user?.id},a="Conversation"===t.__typename?t.conversationTitle??"":t.title;return{id:e.id,title:e.displayName,sourceTitle:a,origin:i,updatedAt:new Date(e.timeUpdated).toISOString(),thumbnailUrl:e.thumbnailUrl??void 0,presentation:{type:"asset",fileType:(0,r.fileTypeFromContentType)(e.contentType??""),contentType:e.contentType??null}}}(e.asset)];case"OrgSearchConversationResult":case"OrgSearchReplResult":return[]}})}])},998524,e=>{"use strict";var t=e.i(351623),r=e.i(805610),i=e.i(344480),a=e.i(975473);let n={},s=t.gql`
    query LibraryResources($orgId: String, $query: String, $after: String, $resourceKinds: [ResourceKind!]!, $filters: OrgSearchFiltersInput) {
  workspaceSearch(
    orgId: $orgId
    resourceKinds: $resourceKinds
    count: 24
    query: $query
    after: $after
    filters: $filters
  ) {
    __typename
    ... on OrgSearchConnection {
      degradedKinds
      items {
        ...LibrarySearchResult
      }
      pageInfo {
        hasNextPage
        nextCursor
      }
    }
    ... on UserError {
      message
    }
    ... on UnauthorizedError {
      message
    }
    ... on ServiceUnavailable {
      message
    }
  }
}
    ${r.LibrarySearchResultFragmentDoc}`;e.s(["useLibraryResourcesLazyQuery",0,function(e){let t={...n,...e};return a.useLazyQuery(s,t)},"useLibraryResourcesQuery",0,function(e){let t={...n,...e};return i.useQuery(s,t)}])},399663,e=>{"use strict";var t=e.i(389959),r=e.i(392771),i=e.i(998524),a=e.i(320216);e.i(214847);var n=e.i(864300),s=e.i(812406),o=e.i(987520);let l=[],u=[];function d(e,t){let r=e?.workspaceSearch;if(r?.__typename==="OrgSearchConnection")return r.degradedKinds.length<t||r.items.length>0?r:void 0}function c(e,t,r,i=null){if(null===r)return{orgId:e,query:t,resourceKinds:["ASSET","ARTIFACT"],filters:null};if(r===o.FILE_TYPE_FILTER){let r=null===i?null:(0,s.assetTypesForLibraryFilter)(i);return{orgId:e,query:t,resourceKinds:["ASSET"],filters:null===r?null:{asset:{assetTypes:r}}}}return{orgId:e,query:t,resourceKinds:["ARTIFACT"],filters:{artifact:{artifactKind:r}}}}e.s(["useLibraryResources",0,function({orgId:e,query:s,typeFilter:p,assetTypeFilter:g=null,creatorFilter:m}){let h=(0,n.useIntl)(),{showError:f}=(0,a.default)(),b=s.trim(),y={...c(e,""===b?null:b,p,g),after:null},S={...y,count:24},v=y.resourceKinds.length,{client:_,data:R,loading:C,error:T,networkStatus:x,fetchMore:I,refetch:A}=(0,i.useLibraryResourcesQuery)({variables:y,fetchPolicy:"cache-and-network",nextFetchPolicy:"cache-first",notifyOnNetworkStatusChange:!0,errorPolicy:"all",ssr:!1}),O=d(R,v),P=!C&&(void 0!==T||void 0!==R&&void 0===O),k=e??"personal",M=JSON.stringify(S),w=(0,t.useRef)(null),E=(0,t.useRef)({entryKey:M,id:0});E.current.entryKey!==M&&(E.current={entryKey:M,id:E.current.id+1});let j=E.current.id,D=(0,t.useCallback)(e=>{_.cache.evict({id:"ROOT_QUERY",fieldName:"workspaceSearch",args:e}),_.cache.gc()},[_]);(0,t.useEffect)(()=>()=>{E.current.entryKey===M&&E.current.id===j&&(E.current={entryKey:"",id:j+1});let e=w.current;e?.key===M&&e.visitId===j&&(w.current=null,D(e.cacheArgs))},[M,D,j]);let L=(0,t.useRef)(null);P?L.current=null:void 0!==O&&(L.current={scopeKey:k,typeFilter:p,assetTypeFilter:g,connection:O});let F=void 0===O&&C&&L.current?.scopeKey===k&&L.current.typeFilter===p&&L.current.assetTypeFilter===g&&L.current.connection.items.length>0?L.current.connection:void 0,G=O??F,U=x===r.NetworkStatus.fetchMore,z=C&&!U&&void 0===G,$=x===r.NetworkStatus.setVariables&&void 0!==G,H=G?(0,o.toLibraryItems)(G.items):l,N=H;void 0!==m&&(N=H.filter(e=>e.origin?.kind==="conversation"?m.creatorId===m.currentUserId:e.origin?.creatorId===m.creatorId));let V=O?.pageInfo.hasNextPage?O.pageInfo.nextCursor:void 0,B=null!=V,W=(0,t.useRef)(new Set);return{items:N,degradedKinds:G?.degradedKinds??u,isLoading:z,isLoadingMore:U,isSearching:$,isError:P,canLoadMore:B,loadMore:()=>{if(C||W.current.has(j)||!V)return;W.current.add(j);let e=()=>{f(h.formatMessage({id:"library.couldNotLoadMore",defaultMessage:"Could not load more results. Please try again."}))};I({variables:{after:V},updateQuery:(e,{fetchMoreResult:t})=>{if(E.current.entryKey!==M||E.current.id!==j)return e;let r=d(e,v),i=d(t,v);return r&&i&&r.pageInfo.nextCursor===V?{...t,workspaceSearch:{...i,degradedKinds:[...new Set([...r.degradedKinds,...i.degradedKinds])],items:[...r.items,...i.items]}}:e}}).then(t=>{void 0===d(t.data,v)?e():E.current.entryKey===M&&E.current.id===j&&(w.current={key:M,visitId:j,cacheArgs:S})},e).finally(()=>{W.current.delete(j)})},retry:()=>{A().catch(()=>void 0)}}},"usePrewarmLibrary",0,function(e,r){let[a]=(0,i.useLibraryResourcesLazyQuery)(),n=(0,t.useRef)(new Set),s=void 0===e?"personal":`org:${e}`;return(0,t.useCallback)(()=>{!r||n.current.has(s)||(n.current.add(s),a({variables:{...c(e,null,o.FILE_TYPE_FILTER),after:null}}))},[a,e,r,s])}])},99357,e=>{"use strict";var t=e.i(351623),r=e.i(667746),i=e.i(444008),a=e.i(820669),n=e.i(576954),s=e.i(130902);e.i(344480),e.i(975473);var o=e.i(299020);let l={},u=t.gql`
    fragment ReplMultiplayerOrgV2 on Org {
  ...OrgMetadata
  membersCount
  adminGroup: groups(input: {types: [system_admins]}) {
    ... on OrgGroupConnection {
      items {
        id
        memberCount
        isMember
      }
    }
  }
  customer {
    __typename
    ... on Customer {
      ...CollaboratorCountV2Customer
      ...InvitePromotionsCustomer
    }
  }
  authorizations {
    editSubscription {
      isAuthorized
    }
  }
}
    ${i.OrgMetadataFragmentDoc}
${a.CollaboratorCountV2CustomerFragmentDoc}
${n.InvitePromotionsCustomerFragmentDoc}`,d=t.gql`
    fragment ReplMultiplayerOrgGroupV2 on OrgGroup {
  ...OrgGroupsOrgGroup
}
    ${s.OrgGroupsOrgGroupFragmentDoc}`,c=t.gql`
    fragment ReplMultiplayerGroupV2 on ReplMultiplayerGroupScope {
  group {
    __typename
    ...ReplMultiplayerOrgGroupV2
  }
  options {
    __typename
    role
    status
  }
}
    ${d}`,p=t.gql`
    fragment ReplMultiplayerIndividualV2 on ReplMultiplayerIndividualScope {
  group {
    __typename
    ...ReplMultiplayerOrgGroupV2
  }
  user {
    __typename
    id
    displayName
    fullName
    image
    username
  }
  options {
    __typename
    role
    status
  }
}
    ${d}`,g=t.gql`
    fragment ReplMultiplayerStatusV2 on Repl {
  id
  title
  isPrivate
  url
  org {
    ...ReplMultiplayerOrgV2
  }
  multiplayerStatus {
    __typename
    groups {
      __typename
      ...ReplMultiplayerGroupV2
    }
    individuals {
      __typename
      ...ReplMultiplayerIndividualV2
    }
    pendingInvites {
      __typename
      email
    }
  }
  authorizations {
    editPermissions {
      isAuthorized
      message
    }
    editVisibility {
      isAuthorized
      message
    }
    removeSelf {
      isAuthorized
      message
    }
    viewPermissions {
      isAuthorized
      message
    }
  }
}
    ${u}
${c}
${p}`,m=t.gql`
    mutation UpdateOrgGroupScopesV2($input: UpdateOrgGroupScopesInput!) {
  updateOrgGroupScopes(input: $input) {
    __typename
    ... on OrgGroup {
      ...OrgGroupPermissionsGroup
    }
    ... on Error {
      message
    }
  }
}
    ${r.OrgGroupPermissionsGroupFragmentDoc}`,h=t.gql`
    mutation GrantReplAccessByEmail($input: GrantReplAccessByEmailInput!) {
  grantReplAccessByEmail(input: $input) {
    ... on Repl {
      __typename
      ...ReplMultiplayerStatusV2
    }
    ... on UnauthorizedError {
      __typename
      message
    }
    ... on NotFoundError {
      __typename
      message
    }
    ... on UserError {
      __typename
      message
    }
  }
}
    ${g}`;e.s(["ReplMultiplayerStatusV2FragmentDoc",0,g,"useGrantReplAccessByEmailMutation",0,function(e){let t={...l,...e};return o.useMutation(h,t)},"useUpdateOrgGroupScopesV2Mutation",0,function(e){let t={...l,...e};return o.useMutation(m,t)}])},576954,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment InvitePromotionsCustomer on Customer {
  id
  authorizations {
    enablePromotions {
      isAuthorized
    }
  }
}
    `;e.s(["InvitePromotionsCustomerFragmentDoc",0,r])},667746,e=>{"use strict";var t=e.i(351623),r=e.i(970410),i=e.i(861326),a=e.i(427144),n=e.i(939307),s=e.i(394352);let o=t.gql`
    fragment OrgGroupPermissionsGroup on OrgGroup {
  ...OrgGroupMetadata
  permissions {
    editPermissions
    viewPermissions
  }
  ...OrgGroupPermissionsOrgGroup
  ...OrgGroupPermissionsGroupsGroup
  ...OrgGroupPermissionsReplsGroup
  ...ReplScopeSelectionGroup
}
    ${r.OrgGroupMetadataFragmentDoc}
${i.OrgGroupPermissionsOrgGroupFragmentDoc}
${a.OrgGroupPermissionsGroupsGroupFragmentDoc}
${n.OrgGroupPermissionsReplsGroupFragmentDoc}
${s.ReplScopeSelectionGroupFragmentDoc}`,l=t.gql`
    fragment OrgGroupPermissionsOrg on Org {
  ...OrgGroupPermissionsOrgOrg
  ...OrgGroupPermissionsGroupsOrg
  ...OrgGroupPermissionsReplsOrg
}
    ${i.OrgGroupPermissionsOrgOrgFragmentDoc}
${a.OrgGroupPermissionsGroupsOrgFragmentDoc}
${n.OrgGroupPermissionsReplsOrgFragmentDoc}`;e.s(["OrgGroupPermissionsGroupFragmentDoc",0,o,"OrgGroupPermissionsOrgFragmentDoc",0,l])},427144,e=>{"use strict";var t=e.i(351623),r=e.i(299020),i=e.i(344480);e.i(975473);let a={},n=t.gql`
    fragment OrgGroupPermissionsGroupsGroup on OrgGroup {
  id
  name
  permissions {
    editPermissions
  }
}
    `,s=t.gql`
    fragment OrgGroupPermissionsGroupsOption on OrgGroup {
  id
  slug
  name
  type
}
    `,o=t.gql`
    fragment OrgGroupPermissionsGroupsOrg on Org {
  id
  slug
}
    `,l=t.gql`
    fragment OrgGroupOption on OrgGroupScopeOption {
  status
  role
}
    `,u=t.gql`
    fragment OrgGroupOptions on OrgGroupScopeOptions {
  targetGroupId
  options {
    __typename
    ...OrgGroupOption
  }
}
    ${l}`,d=t.gql`
    mutation UpdateOrgGroupGroupScopes($input: UpdateOrgGroupScopesInput!) {
  updateOrgGroupScopes(input: $input) {
    __typename
    ... on OrgGroup {
      ...OrgGroupPermissionsGroupsGroup
    }
    ... on Error {
      message
    }
  }
}
    ${n}`,c=t.gql`
    query OrgGroupPermissionsGroups($orgId: String, $input: OrgGroupsInput) {
  currentUser {
    __typename
    id
    org(orgId: $orgId) {
      __typename
      ... on Org {
        id
        groups(input: $input) {
          __typename
          ... on OrgGroupConnection {
            pageInfo {
              hasNextPage
              nextCursor
            }
            items {
              ...OrgGroupPermissionsGroupsOption
            }
          }
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
    ${s}`,p=t.gql`
    query OrgGroupScopeOptions($orgId: String!, $groupId: String!, $input: OrgGroupGroupScopeOptionsInput!) {
  currentUser {
    __typename
    id
    org(orgId: $orgId) {
      __typename
      ... on Org {
        id
        group(orgGroupId: $groupId) {
          __typename
          ... on OrgGroup {
            __typename
            id
            groupScopeOptions(input: $input) {
              __typename
              ... on OrgGroupGroupScopeOptions {
                items {
                  ...OrgGroupOptions
                }
              }
              ... on UnauthorizedError {
                message
              }
            }
          }
          ... on NotFoundError {
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
    ${u}`;e.s(["OrgGroupPermissionsGroupsGroupFragmentDoc",0,n,"OrgGroupPermissionsGroupsOrgFragmentDoc",0,o,"useOrgGroupPermissionsGroupsQuery",0,function(e){let t={...a,...e};return i.useQuery(c,t)},"useOrgGroupScopeOptionsQuery",0,function(e){let t={...a,...e};return i.useQuery(p,t)},"useUpdateOrgGroupGroupScopesMutation",0,function(e){let t={...a,...e};return r.useMutation(d,t)}])},861326,e=>{"use strict";var t=e.i(351623),r=e.i(299020);let i={},a=t.gql`
    fragment OrgGroupPermissionsOrgScopeOption on OrgScopeOption {
  __typename
  role
  status
}
    `,n=t.gql`
    fragment OrgGroupPermissionsOrgGroup on OrgGroup {
  id
  permissions {
    editPermissions
  }
  orgScopeOptions {
    ...OrgGroupPermissionsOrgScopeOption
  }
}
    ${a}`,s=t.gql`
    fragment OrgGroupPermissionsOrgOrg on Org {
  id
  name
}
    `,o=t.gql`
    mutation UpdateOrgGroupOrgScopes($input: UpdateOrgGroupScopesInput!) {
  updateOrgGroupScopes(input: $input) {
    __typename
    ... on OrgGroup {
      ...OrgGroupPermissionsOrgGroup
    }
    ... on Error {
      message
    }
  }
}
    ${n}`;e.s(["OrgGroupPermissionsOrgGroupFragmentDoc",0,n,"OrgGroupPermissionsOrgOrgFragmentDoc",0,s,"useUpdateOrgGroupOrgScopesMutation",0,function(e){let t={...i,...e};return r.useMutation(o,t)}])},939307,e=>{"use strict";var t=e.i(351623),r=e.i(319801),i=e.i(344480);e.i(975473);let a={},n=t.gql`
    fragment OrgGroupPermissionsReplsGroup on OrgGroup {
  id
  name
}
    `,s=t.gql`
    fragment OrgGroupPermissionsReplsOrg on Org {
  id
}
    `,o=t.gql`
    fragment OrgGroupRepl on Repl {
  id
  title
  iconUrl
  ...ReplLinkRepl
  authorizations {
    editPermissions {
      isAuthorized
    }
  }
}
    ${r.ReplLinkReplFragmentDoc}`,l=t.gql`
    fragment GroupReplsPage on ReplConnection {
  pageInfo {
    hasNextPage
    nextCursor
    previousCursor
  }
  items {
    __typename
    ...OrgGroupRepl
  }
}
    ${o}`,u=t.gql`
    fragment ReplScopeOption on ReplScopeOption {
  status
  role
}
    `,d=t.gql`
    fragment ReplScopeOptions on ReplScopeOptions {
  replId
  options {
    __typename
    ...ReplScopeOption
  }
}
    ${u}`,c=t.gql`
    query OrgGroupPermissionsRepls($orgId: String, $input: OrgReplsInput!) {
  currentUser {
    __typename
    id
    org(orgId: $orgId) {
      __typename
      ... on Org {
        id
        replsCount
        repls(input: $input) {
          __typename
          ... on ReplConnection {
            ...GroupReplsPage
          }
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
    ${l}`,p=t.gql`
    query OrgGroupReplScopeOptions($orgId: String!, $groupId: String!, $input: OrgGroupReplScopeOptionsInput!) {
  currentUser {
    __typename
    id
    org(orgId: $orgId) {
      __typename
      ... on Org {
        id
        group(orgGroupId: $groupId) {
          __typename
          ... on OrgGroup {
            __typename
            id
            replScopeOptions(input: $input) {
              __typename
              ... on OrgGroupReplScopeOptions {
                items {
                  ...ReplScopeOptions
                }
              }
              ... on UnauthorizedError {
                message
              }
            }
          }
          ... on NotFoundError {
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
    ${d}`;e.s(["OrgGroupPermissionsReplsGroupFragmentDoc",0,n,"OrgGroupPermissionsReplsOrgFragmentDoc",0,s,"useOrgGroupPermissionsReplsQuery",0,function(e){let t={...a,...e};return i.useQuery(c,t)},"useOrgGroupReplScopeOptionsQuery",0,function(e){let t={...a,...e};return i.useQuery(p,t)}])},394352,e=>{"use strict";var t=e.i(351623),r=e.i(299020);let i={},a=t.gql`
    fragment ReplScopeSelectionGroup on OrgGroup {
  id
  name
}
    `,n=t.gql`
    mutation UpdateOrgGroupReplScopes($input: UpdateOrgGroupScopesInput!) {
  updateOrgGroupScopes(input: $input) {
    __typename
    ... on OrgGroup {
      id
    }
    ... on Error {
      message
    }
  }
}
    `;e.s(["ReplScopeSelectionGroupFragmentDoc",0,a,"useUpdateOrgGroupReplScopesMutation",0,function(e){let t={...i,...e};return r.useMutation(n,t)}])},289038,e=>{"use strict";var t=e.i(276385),r=e.i(138716),i=e.i(995691),a=e.i(255701),n=e.i(612343);e.i(214847);var s=e.i(864300),o=e.i(776065),l=e.i(448942);let u=e=>t=>t.pathname===e;e.s(["useOrgGroupNavItems",0,function({orgSlug:e}){let d=(0,s.useIntl)(),c=(0,o.useQueryParam)("groupId","string"),p=(0,o.useQueryParam)("groupSlug","string");if(!e||!c||!p)return[];let g=(0,l.orgLinks)({slug:e}),m=(0,l.orgGroupLinks)({orgSlug:e,groupId:c,groupSlug:p});return[{label:d.formatMessage({id:"orgs.groupSidebarBack",defaultMessage:"Back"}),href:g.groups.href.toString(),icon:(0,t.jsx)(r.default,{}),active:u(g.groups.routerPath)},{label:d.formatMessage({id:"orgs.groupSidebarMembers",defaultMessage:"Members"}),href:m.members.href.toString(),icon:(0,t.jsx)(n.default,{}),active:u(m.members.routerPath)},{label:d.formatMessage({id:"orgs.groupSidebarPermissions",defaultMessage:"Permissions"}),href:m.permissions.href.toString(),icon:(0,t.jsx)(i.default,{}),active:u(m.permissions.routerPath)},{label:d.formatMessage({id:"orgs.groupSidebarSettings",defaultMessage:"Group settings"}),href:m.settings.href.toString(),icon:(0,t.jsx)(a.default,{}),active:u(m.settings.routerPath)}]}])},669011,e=>{"use strict";var t=e.i(351623),r=e.i(99357),i=e.i(344480);e.i(975473);let a={},n=t.gql`
    fragment ReplEnvironmentHeaderInviteButtonRepl on Repl {
  id
  owner {
    ... on User {
      id
    }
    ... on Team {
      id
    }
  }
  org {
    id
  }
  authorizations {
    editPermissions {
      isAuthorized
    }
  }
}
    `,s=t.gql`
    query InviteButtonOrgReplPerms($replId: String!) {
  currentUser {
    id
    username
    fullName
    image
  }
  getRepl(id: $replId) {
    __typename
    ... on Repl {
      __typename
      ...ReplMultiplayerStatusV2
    }
  }
}
    ${r.ReplMultiplayerStatusV2FragmentDoc}`;e.s(["ReplEnvironmentHeaderInviteButtonReplFragmentDoc",0,n,"useInviteButtonOrgReplPermsQuery",0,function(e){let t={...a,...e};return i.useQuery(s,t)}])},663197,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment ReplInfoRepl on Repl {
  id
  title
  iconUrl
}
    `;e.s(["ReplInfoReplFragmentDoc",0,r])},951894,e=>{"use strict";var t=e.i(351623),r=e.i(663197),i=e.i(669011);let a=t.gql`
    fragment ReplEnvironmentHeaderRepl on Repl {
  id
  authorizations {
    viewPermissions {
      isAuthorized
    }
    viewDeploymentConfig {
      isAuthorized
    }
    viewFreemiumExperience {
      isAuthorized
    }
  }
  config {
    isInPlanningPhase
  }
  ...ReplInfoRepl
  ...ReplEnvironmentHeaderInviteButtonRepl
}
    ${r.ReplInfoReplFragmentDoc}
${i.ReplEnvironmentHeaderInviteButtonReplFragmentDoc}`;e.s(["ReplEnvironmentHeaderReplFragmentDoc",0,a])},122126,e=>{"use strict";var t=e.i(351623),r=e.i(458511);let i=t.gql`
    fragment ReplEnvironmentDesktopReplInfraHooksRepl on Repl {
  id
  slug
  org {
    id
  }
  origin {
    id
  }
  language
  user {
    id
  }
  authorizations {
    editFileContents {
      isAuthorized
    }
  }
  ...WorkspaceToolPanesRepl
}
    ${r.WorkspaceToolPanesReplFragmentDoc}`,a=t.gql`
    fragment ReplEnvironmentDesktopReplInfraHooksCurrentUser on CurrentUser {
  id
  username
  isAdmin: hasRole(role: ADMIN)
}
    `;e.s(["ReplEnvironmentDesktopReplInfraHooksCurrentUserFragmentDoc",0,a,"ReplEnvironmentDesktopReplInfraHooksReplFragmentDoc",0,i])},809541,e=>{"use strict";var t=e.i(351623),r=e.i(29530),i=e.i(922819),a=e.i(122126);let n=t.gql`
    fragment ReplEnvironmentDesktopTopLevelHooksRepl on Repl {
  id
  ...PageTitleRepl
  ...ReplEnvironmentDesktopAgentSessionHooksRepl
  ...ReplEnvironmentDesktopReplInfraHooksRepl
}
    ${r.PageTitleReplFragmentDoc}
${i.ReplEnvironmentDesktopAgentSessionHooksReplFragmentDoc}
${a.ReplEnvironmentDesktopReplInfraHooksReplFragmentDoc}`,s=t.gql`
    fragment ReplEnvironmentDesktopTopLevelHooksCurrentUser on CurrentUser {
  hasUserCompletedOnboarding
  timeCreated
  ...ReplEnvironmentDesktopReplInfraHooksCurrentUser
}
    ${a.ReplEnvironmentDesktopReplInfraHooksCurrentUserFragmentDoc}`;e.s(["ReplEnvironmentDesktopTopLevelHooksCurrentUserFragmentDoc",0,s,"ReplEnvironmentDesktopTopLevelHooksReplFragmentDoc",0,n])},913864,e=>{"use strict";var t=e.i(351623),r=e.i(383889),i=e.i(764992),a=e.i(809541),n=e.i(575872),s=e.i(951894),o=e.i(147381),l=e.i(879106),u=e.i(689736);let d=t.gql`
    fragment ReplEnvironmentDesktopCurrentUser on CurrentUser {
  id
  ...WorkspaceAdminRoles
  ...CrosisContextCurrentUser
  ...ReplEnvironmentDesktopTopLevelHooksCurrentUser
  workspacePreferences
  ...WorkspaceDoGitProviderImportCurrentUser
}
    ${r.WorkspaceAdminRolesFragmentDoc}
${i.CrosisContextCurrentUserFragmentDoc}
${a.ReplEnvironmentDesktopTopLevelHooksCurrentUserFragmentDoc}
${n.WorkspaceDoGitProviderImportCurrentUserFragmentDoc}`,c=t.gql`
    fragment WorkspaceConnectAuthorization on ReplAuthorizations {
  connectToWorkspace {
    isAuthorized
    code
  }
}
    `,p=t.gql`
    fragment ReplEnvironmentDesktopRepl on Repl {
  id
  layoutState
  initialEditorMode
  description
  config {
    isAgentStack
    isWebDesignMockup
    zealot
    agentHarness
  }
  ...CrosisContextRepl
  authorizations {
    ...WorkspaceConnectAuthorization
  }
  ...WorkspaceDoGitProviderImportRepl
  ...ReplEnvironmentHeaderRepl
  ...FigmaImportFlowRepl
  ...LovableZipImportFlowRepl
  ...ReplEnvironmentDesktopTopLevelHooksRepl
  ...HandleReplRedirectionRepl
}
    ${i.CrosisContextReplFragmentDoc}
${c}
${n.WorkspaceDoGitProviderImportReplFragmentDoc}
${s.ReplEnvironmentHeaderReplFragmentDoc}
${o.FigmaImportFlowReplFragmentDoc}
${l.LovableZipImportFlowReplFragmentDoc}
${a.ReplEnvironmentDesktopTopLevelHooksReplFragmentDoc}
${u.HandleReplRedirectionReplFragmentDoc}`;e.s(["ReplEnvironmentDesktopCurrentUserFragmentDoc",0,d,"ReplEnvironmentDesktopReplFragmentDoc",0,p,"WorkspaceConnectAuthorizationFragmentDoc",0,c])},147381,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var i=e.i(299020);let a={},n=t.gql`
    fragment FigmaImportFlowRepl on Repl {
  id
  config {
    figmaUrl
    figmaImportStatus
  }
}
    `,s=t.gql`
    query FigmaImportFlow($replId: String!) {
  getRepl(id: $replId) {
    ... on Repl {
      id
      ...FigmaImportFlowRepl
    }
  }
}
    ${n}`,o=t.gql`
    mutation UpdateReplForFigmaImport($input: UpdateReplInput!) {
  updateRepl(input: $input) {
    repl {
      id
      isRenamed
      title
      ...FigmaImportFlowRepl
    }
  }
}
    ${n}`,l=t.gql`
    mutation GetFigmaIntermediateToken($input: GetFigmaIntermediateTokenInput!) {
  getFigmaIntermediateToken(input: $input) {
    ... on FigmaImportIntermediateToken {
      token
    }
    ... on NoOAuthFoundError {
      message
    }
    ... on InvalidFigmaUrlError {
      message
    }
    ... on FigmaRateLimitError {
      message
    }
  }
}
    `;e.s(["FigmaImportFlowReplFragmentDoc",0,n,"useFigmaImportFlowQuery",0,function(e){let t={...a,...e};return r.useQuery(s,t)},"useGetFigmaIntermediateTokenMutation",0,function(e){let t={...a,...e};return i.useMutation(l,t)},"useUpdateReplForFigmaImportMutation",0,function(e){let t={...a,...e};return i.useMutation(o,t)}])},689736,e=>{"use strict";var t=e.i(351623),r=e.i(319801),i=e.i(344480);e.i(975473);let a={},n=t.gql`
    fragment HandleReplRedirectionReplRedirect on ReplRedirect {
  replUrl
}
    `,s=t.gql`
    fragment HandleReplRedirectionSubscriptionExpired on ReplSubscriptionExpired {
  isOwner
  replId
}
    `,o=t.gql`
    fragment HandleReplRedirectionRepl on Repl {
  id
  language
  ...ReplLinkRepl
  authorizations {
    connectToWorkspace {
      isAuthorized
    }
  }
  org {
    id
  }
}
    ${r.ReplLinkReplFragmentDoc}`,l=t.gql`
    query HandleReplRedirection($replId: String!) {
  getRepl(id: $replId) {
    ... on Repl {
      ...HandleReplRedirectionRepl
      heliumMigrationStatus {
        isUsingHelium
        shouldMigrateToHelium
      }
    }
    ... on ReplRedirect {
      ...HandleReplRedirectionReplRedirect
    }
    ... on ReplSubscriptionExpired {
      ...HandleReplRedirectionSubscriptionExpired
    }
  }
}
    ${o}
${n}
${s}`;e.s(["HandleReplRedirectionReplFragmentDoc",0,o,"HandleReplRedirectionReplRedirectFragmentDoc",0,n,"HandleReplRedirectionSubscriptionExpiredFragmentDoc",0,s,"useHandleReplRedirectionQuery",0,function(e){let t={...a,...e};return i.useQuery(l,t)}])},29530,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment PageTitleRepl on Repl {
  id
  title
}
    `;e.s(["PageTitleReplFragmentDoc",0,r])},922819,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment ReplEnvironmentDesktopAgentSessionHooksRepl on Repl {
  id
  config {
    isInPlanningPhase
  }
  origin {
    id
  }
}
    `;e.s(["ReplEnvironmentDesktopAgentSessionHooksReplFragmentDoc",0,r])},575872,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var i=e.i(299020);let a={},n=t.gql`
    fragment WorkspaceDoGitProviderImportCurrentUser on CurrentUser {
  id
  username
}
    `,s=t.gql`
    fragment WorkspaceDoGitProviderImportRepl on Repl {
  id
  org {
    id
  }
  config {
    gitRemoteUrl
    doClone
    isAgentRepl
  }
}
    `,o=t.gql`
    query WorkspaceDoGitProviderImportFlow($replId: String!) {
  getRepl(id: $replId) {
    ...WorkspaceDoGitProviderImportRepl
  }
  currentUser {
    ...WorkspaceDoGitProviderImportCurrentUser
  }
}
    ${s}
${n}`,l=t.gql`
    query WorkspaceDoGitProviderImportConnectorBaseUrl($input: GitProviderContextInput) {
  currentUser {
    id
    gitHubInfoV2(input: $input) {
      webBaseUrl
      apiBaseUrl
    }
  }
}
    `,u=t.gql`
    mutation WorkspaceDoGitProviderImportFlowFinishClone($input: UpdateReplInput!) {
  updateRepl(input: $input) {
    repl {
      id
      config {
        doClone
      }
    }
  }
}
    `,d=t.gql`
    query GitHubImportGetTopLanguages($input: GitHubRepoInfoInput!) {
  githubRepoInfo(input: $input) {
    ... on GitHubRepoInfoOutput {
      topLanguages
      repositorySource
    }
    ... on TooManyRequestsError {
      message
    }
    ... on UnauthorizedError {
      message
    }
    ... on ServiceUnavailable {
      message
    }
    ... on UserError {
      message
    }
    ... on NotFoundError {
      message
    }
  }
}
    `,c=t.gql`
    query BitbucketImportGetTopLanguages($input: BitbucketRepoInfoInput!) {
  bitbucketRepoInfo(input: $input) {
    ... on BitbucketRepoInfoOutput {
      topLanguages
      repositorySource
    }
    ... on UnauthorizedError {
      message
    }
    ... on ServiceUnavailable {
      message
    }
    ... on UserError {
      message
    }
    ... on NotFoundError {
      message
    }
  }
}
    `;e.s(["WorkspaceDoGitProviderImportCurrentUserFragmentDoc",0,n,"WorkspaceDoGitProviderImportReplFragmentDoc",0,s,"useBitbucketImportGetTopLanguagesQuery",0,function(e){let t={...a,...e};return r.useQuery(c,t)},"useGitHubImportGetTopLanguagesQuery",0,function(e){let t={...a,...e};return r.useQuery(d,t)},"useWorkspaceDoGitProviderImportConnectorBaseUrlQuery",0,function(e){let t={...a,...e};return r.useQuery(l,t)},"useWorkspaceDoGitProviderImportFlowFinishCloneMutation",0,function(e){let t={...a,...e};return i.useMutation(u,t)},"useWorkspaceDoGitProviderImportFlowQuery",0,function(e){let t={...a,...e};return r.useQuery(o,t)}])},879106,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var i=e.i(299020),a=e.i(846545);let n={},s=t.gql`
    fragment LovableZipImportFlowRepl on Repl {
  id
  config {
    zipImportSource
    zipImportStatus
  }
}
    `,o=t.gql`
    query LovableZipImportFlow($replId: String!) {
  getRepl(id: $replId) {
    ... on Repl {
      id
      ...LovableZipImportFlowRepl
    }
  }
}
    ${s}`,l=t.gql`
    mutation UpdateReplForLovableZipImport($input: UpdateReplInput!) {
  updateRepl(input: $input) {
    repl {
      id
      ...LovableZipImportFlowRepl
    }
  }
}
    ${s}`,u=t.gql`
    subscription LovableImportZipProgress($replId: String!) {
  importZipProgress(replId: $replId)
}
    `;e.s(["LovableZipImportFlowReplFragmentDoc",0,s,"useLovableImportZipProgressSubscription",0,function(e){let t={...n,...e};return a.useSubscription(u,t)},"useLovableZipImportFlowQuery",0,function(e){let t={...n,...e};return r.useQuery(o,t)},"useUpdateReplForLovableZipImportMutation",0,function(e){let t={...n,...e};return i.useMutation(l,t)}])},383889,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let i={},a=t.gql`
    fragment WorkspaceAdminRoles on CurrentUser {
  isAdmin: hasRole(role: ADMIN)
  isStaff: hasRole(role: REPLIT_STAFF)
}
    `,n=t.gql`
    query WorkspaceShowAdmin {
  currentUser {
    id
    ...WorkspaceAdminRoles
  }
}
    ${a}`;e.s(["WorkspaceAdminRolesFragmentDoc",0,a,"useWorkspaceShowAdminQuery",0,function(e){let t={...i,...e};return r.useQuery(n,t)}])},458511,e=>{"use strict";var t=e.i(351623);let r=t.gql`
    fragment WorkspaceToolPanesRepl on Repl {
  authorizations {
    editPermissions {
      isAuthorized
    }
    viewDeploymentConfig {
      isAuthorized
    }
  }
  config {
    initialStackBlueprint
  }
}
    `;e.s(["WorkspaceToolPanesReplFragmentDoc",0,r])},959485,e=>{"use strict";e.i(242933);var t=e.i(790164),r=e.i(729245),i=e.i(854246);function a(e){return e?{client:e.client,sessionId:e.sessionId}:null}function n(e,t){return e.endpoint===t.endpoint}function s(){return"u">typeof document&&"visible"===document.visibilityState}class o{runtime=null;feeds=new Map;statuses=new Map;snapshots=new Map;snapshotTargets=new Map;backends=new Map;connections=new Map;activeClientReplId=null;documentVisible=new t.ObservableState(s());detachVisibility=null;detachActiveTaskSource=null;activeReplId=null;isActiveReplReady=!1;connect(e){this.disconnectRuntime(),this.runtime=e;let t=()=>this.documentVisible.set(s());for(let e of(document.addEventListener("visibilitychange",t),this.documentVisible.set(s()),this.detachVisibility=()=>document.removeEventListener("visibilitychange",t),this.activeReplId=r.activeTaskSource.current?.replId??null,this.isActiveReplReady=r.activeTaskSource.current?.isReady??!1,this.publishActiveConnection(r.activeTaskSource.current?.replId??null,a(r.activeTaskSource.current)),this.detachActiveTaskSource=r.activeTaskSource.subscribe(e=>{this.publishActiveConnection(e?.replId??null,a(e)),this.setActiveTaskSource(e?.replId??null,e?.isReady??!1)}),this.feeds.values()))this.start(e);return()=>{this.runtime===e&&this.disconnectRuntime()}}acquire(e){let{replId:r}=e,i=this.feeds.get(r);if(i)n(i.target,e)||(this.disconnect(i),i.target=e,this.backendState(r).set("cell"),this.clearSnapshot(r),this.start(i));else{let a=this.snapshotTargets.get(r);a&&n(a,e)||this.clearSnapshot(r);let s=new t.ObservableState(this.documentVisible.current);i={target:e,refCount:0,active:s,detachActivity:this.documentVisible.subscribe(e=>s.set(e)),disconnect:null},this.feeds.set(r,i),this.backendState(r).set("cell"),this.start(i)}i.refCount+=1;let a=!1;return()=>{if(a)return;a=!0;let e=this.feeds.get(r);e===i&&(e.refCount-=1,0===e.refCount&&this.stop(r,e))}}restart(e){let t=this.feeds.get(e);t&&"error"===this.statusState(e).current&&(this.disconnect(t),this.backendState(e).set("cell"),this.start(t))}statusFor(e){return this.statusState(e)}hasSnapshotFor(e){return this.snapshotState(e)}backendFor(e){return this.backendState(e)}connectionFor(e){return this.connectionState(e)}publishActiveConnection(e,t){let r=this.activeClientReplId;null!==r&&r!==e&&this.connectionState(r).set(null),this.activeClientReplId=e,null!==e&&this.connectionState(e).set(t)}connectionState(e){let r=this.connections.get(e);if(r)return r;let i=new t.ObservableState(null);return this.connections.set(e,i),i}backendState(e){let r=this.backends.get(e);if(r)return r;let i=new t.ObservableState(null);return this.backends.set(e,i),i}statusState(e){let r=this.statuses.get(e);if(r)return r;let i=new t.ObservableState(null);return this.statuses.set(e,i),i}setStatus(e,t){this.statusState(e).set(t)}snapshotState(e){let r=this.snapshots.get(e);if(r)return r;let i=new t.ObservableState(!1);return this.snapshots.set(e,i),i}markSnapshot(e,t){this.snapshotTargets.set(e,t),this.snapshotState(e).set(!0)}clearSnapshot(e){this.snapshotTargets.delete(e),this.snapshotState(e).set(!1)}start(e){let{target:t}=e,{replId:r}=t,a=this.runtime;if(!a)return void this.setStatus(r,"connecting");if(this.activeReplId===r)return void this.setStatus(r,this.isActiveReplReady?"live":"connecting");this.setStatus(r,"connecting");let n=new AbortController,s=()=>{},o=!1,l=!1,u=0,d=()=>{u+=1,s(),s=()=>{},l=!1,this.activeClientReplId!==r&&this.connectionState(r).set(null)},c=()=>{this.feeds.get(r)===e&&(o=!0,d(),this.setStatus(r,"error"))},p=()=>{let o=a.services.taskman.getSnapshot(m);if(l||!o||!o.transport.sessions.has(o.serverId))return;l=!0;let d=++u,p=()=>this.feeds.get(r)===e&&u===d,g=()=>{p()&&(this.markSnapshot(r,t),this.setStatus(r,"live"))};s=i.teamworkSyncController.startSession({client:o.client,replId:r,externalAbortSignal:n.signal,onSchemaValidationError:()=>{p()&&(c(),o.error())},excludePlans:this.runtime?.lazyPlans??!1,onSnapshot:g}),this.connectionState(r).set({client:o.client,sessionId:o.transport.sessions.get(o.serverId)?.id??null})},g=t=>{this.feeds.get(r)!==e||this.activeReplId===r||o||("closed"===t?(d(),this.setStatus(r,"connecting")):p())},m={...t,userId:a.userId,isClientActiveObservable:e.active,callbacks:{onEndpointResolved:()=>{},onRetryBudgetExhausted:c,onTerminalError:()=>{c(),this.clearSnapshot(r),this.backendState(r).set(null)},onSessionStatusChange:g}},h=a.services.taskman.subscribe(m,p);p(),e.disconnect=()=>{n.abort(),h(),d()}}stop(e,t){this.feeds.delete(e),this.disconnect(t),t.detachActivity(),t.active.set(!1),this.setStatus(e,null)}disconnect(e){e.disconnect?.(),e.disconnect=null}setActiveTaskSource(e,t){let r=this.activeReplId;if(r===e&&this.isActiveReplReady===t)return;this.activeReplId=e,this.isActiveReplReady=t;let i=e?this.feeds.get(e):void 0;if(e&&i&&(this.disconnect(i),t&&this.markSnapshot(e,i.target),this.setStatus(e,t?"live":"connecting")),r&&r!==e){let e=this.feeds.get(r);e&&this.start(e)}}disconnectRuntime(){for(let[e,t]of this.feeds)this.disconnect(t),this.setStatus(e,"connecting");this.detachVisibility?.(),this.detachVisibility=null,this.detachActiveTaskSource?.(),this.detachActiveTaskSource=null,this.publishActiveConnection(null,null),this.activeReplId=null,this.isActiveReplReady=!1,this.runtime=null}}let l=new o;e.s(["taskFeedManager",0,l])},272719,e=>{"use strict";var t=e.i(389959),r=e.i(340970),i=e.i(959485),a=e.i(933302);e.s(["useTaskFeedRuntimeBinder",0,function(e){let n=(0,r.useCellServicesOrNull)(),s=(0,a.useGetFeatureGate)("gate_taskman_lazy_plans",!1),o=(0,t.useMemo)(()=>s(),[s]);(0,t.useEffect)(()=>{if(null!==n&&null!==e)return i.taskFeedManager.connect({services:n,userId:e,lazyPlans:o})},[o,n,e])}])},908833,e=>{"use strict";e.i(81282);var t=e.i(750185);let r=t.Type.Object({description:t.Type.Optional(t.Type.String({description:"A short model- and user-facing summary of what this artifact is and what it is for."})),domains:t.Type.Optional(t.Type.Array(t.Type.String())),id:t.Type.Optional(t.Type.String({description:"A stable identifier for the artifact that persists across folder renames. When absent, the folder name is used as the artifact identity."})),integratedSkills:t.Type.Optional(t.Type.Array(t.Type.Object({name:t.Type.Optional(t.Type.String()),version:t.Type.Optional(t.Type.String())}))),kind:t.Type.Optional(t.Type.Union([t.Type.Literal("web"),t.Type.Literal("slides"),t.Type.Literal("video"),t.Type.Literal("mobile"),t.Type.Literal("automation"),t.Type.Literal("game"),t.Type.Literal("data-app"),t.Type.Literal("cli"),t.Type.Literal("vnc"),t.Type.Literal("api"),t.Type.Literal("design"),t.Type.Literal("custom"),t.Type.Literal("design-system")])),pageMetadata:t.Type.Optional(t.Type.Array(t.Type.Object({path:t.Type.String(),screenshotTimestamp:t.Type.Optional(t.Type.String()),screenshotUri:t.Type.Optional(t.Type.String())}))),previewDomain:t.Type.Optional(t.Type.String()),previewPath:t.Type.Optional(t.Type.String()),previewPort:t.Type.Optional(t.Type.Integer({description:"The port that the workspace preview connects to. Only valid when router is port."})),router:t.Type.Optional(t.Type.Union([t.Type.Literal("port"),t.Type.Literal("path"),t.Type.Literal("domain"),t.Type.Literal("expo-domain")],{description:"The preview routing strategy. port (legacy), path, domain, or expo-domain."})),services:t.Type.Optional(t.Type.Array(t.Type.Object({development:t.Type.Object({run:t.Type.Union([t.Type.Object({args:t.Type.Array(t.Type.String()),env:t.Type.Optional(t.Type.Record(t.Type.String(),t.Type.String()))},{additionalProperties:!1}),t.Type.String(),t.Type.Array(t.Type.String())],{description:"The command to run the service during development."})},{description:"Configuration for running the service during development."}),ensurePreviewReachable:t.Type.Optional(t.Type.String({description:"Path to ensure is reachable before showing the preview."})),env:t.Type.Optional(t.Type.Record(t.Type.String(),t.Type.String(),{description:"Service-wide environment variables. Individual commands can also have their own envs."})),localPort:t.Type.Integer({description:"The port the service listens on."}),name:t.Type.String({description:"The service name, which becomes the workflow name for development."}),paths:t.Type.Optional(t.Type.Array(t.Type.String(),{description:"Path prefixes that will be routed to this service."})),production:t.Type.Optional(t.Type.Object({build:t.Type.Optional(t.Type.Union([t.Type.Object({args:t.Type.Array(t.Type.String()),env:t.Type.Optional(t.Type.Record(t.Type.String(),t.Type.String()))},{additionalProperties:!1}),t.Type.String(),t.Type.Array(t.Type.String())],{description:"Command to build the project for production."})),health:t.Type.Optional(t.Type.Object({liveness:t.Type.Optional(t.Type.Object({headers:t.Type.Optional(t.Type.Record(t.Type.String(),t.Type.String(),{description:"An optional map of HTTP headers that should be added to the readiness check."})),path:t.Type.Optional(t.Type.String({description:"The path for the check. For example, `/ready`. Defaults to `/`."}))})),startup:t.Type.Optional(t.Type.Object({headers:t.Type.Optional(t.Type.Record(t.Type.String(),t.Type.String(),{description:"An optional map of HTTP headers that should be added to the readiness check."})),path:t.Type.Optional(t.Type.String({description:"The path for the check. For example, `/ready`. Defaults to `/`."}))}))},{description:"Configuration for production healthchecks."})),publicDir:t.Type.Optional(t.Type.String({description:"Root directory for static file serving. Only valid when serve is 'static'."})),responseHeaders:t.Type.Optional(t.Type.Array(t.Type.Object({name:t.Type.Optional(t.Type.String({description:"The name of the header to add."})),path:t.Type.Optional(t.Type.String({description:"The pattern to apply the header to."})),value:t.Type.Optional(t.Type.String({description:"The value of the header to add."}))}),{description:"Headers to apply to responses for static serving. Only valid when serve is 'static'."})),rewrites:t.Type.Optional(t.Type.Array(t.Type.Object({from:t.Type.Optional(t.Type.String({description:"The pattern to rewrite."})),to:t.Type.Optional(t.Type.String({description:"The new pattern."}))}),{description:"URL rewrite rules for static serving (e.g. SPA fallback). Only valid when serve is 'static'."})),run:t.Type.Optional(t.Type.Union([t.Type.Object({args:t.Type.Array(t.Type.String()),env:t.Type.Optional(t.Type.Record(t.Type.String(),t.Type.String()))},{additionalProperties:!1}),t.Type.String(),t.Type.Array(t.Type.String())],{description:"Command to run the service in production."})),serve:t.Type.Optional(t.Type.Union([t.Type.Literal("static"),t.Type.Literal("proxy")],{default:"proxy",description:"How the service is served in production. 'proxy' (default) proxies to a running process. 'static' serves files directly from publicDir."}))},{description:"Configuration for building and running the service in production."}))}))),title:t.Type.Optional(t.Type.String()),version:t.Type.Optional(t.Type.String())},{$id:"Artifact"});e.s(["ArtifactSchema",0,r])},143524,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{d:"M7.9 2.25a2.75 2.75 0 0 1 2.312 1.23l.81 1.2.004.007a1.25 1.25 0 0 0 1.044.563H20A2.75 2.75 0 0 1 22.75 8v10A2.75 2.75 0 0 1 20 20.75H4A2.75 2.75 0 0 1 1.25 18v-1a.75.75 0 0 1 1.5 0v1A1.25 1.25 0 0 0 4 19.25h16A1.25 1.25 0 0 0 21.25 18V8A1.25 1.25 0 0 0 20 6.75h-7.93a2.75 2.75 0 0 1-2.297-1.237L8.97 4.319l-.005-.007a1.25 1.25 0 0 0-1.057-.562H4A1.25 1.25 0 0 0 2.75 5v4a.75.75 0 1 1-1.5 0V5A2.75 2.75 0 0 1 4 2.25z"}),(0,t.jsx)("path",{d:"M8.47 9.47a.75.75 0 0 1 1.06 0l3 3a.8.8 0 0 1 .162.243q.015.04.026.082.009.028.016.057l.002.014a1 1 0 0 1 .014.134.75.75 0 0 1-.137.43 1 1 0 0 1-.083.1l-3 3a.75.75 0 1 1-1.06-1.06l1.72-1.72H2a.75.75 0 0 1 0-1.5h8.19l-1.72-1.72a.75.75 0 0 1 0-1.06"})]})}])},406871,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 1.25c5.937 0 10.75 4.813 10.75 10.75S17.937 22.75 12 22.75 1.25 17.937 1.25 12 6.063 1.25 12 1.25M11.996 6a.75.75 0 0 0-.75.75v4.504H6.75a.75.75 0 0 0 0 1.5h4.496v4.496a.75.75 0 0 0 1.5 0v-4.496h4.504a.75.75 0 0 0 0-1.5h-4.504V6.75a.75.75 0 0 0-.75-.75",clipRule:"evenodd"})})}])},996009,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{d:"M16 6.25a.75.75 0 0 1 .75.75v9a.75.75 0 0 1-1.5 0V7a.75.75 0 0 1 .75-.75M8 6.25a.75.75 0 0 1 .75.75v7a.75.75 0 0 1-1.5 0V7A.75.75 0 0 1 8 6.25M12 6.25a.75.75 0 0 1 .75.75v4a.75.75 0 0 1-1.5 0V7a.75.75 0 0 1 .75-.75"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M19 2.25A2.75 2.75 0 0 1 21.75 5v14A2.75 2.75 0 0 1 19 21.75H5A2.75 2.75 0 0 1 2.25 19V5A2.75 2.75 0 0 1 5 2.25zM5 3.75c-.69 0-1.25.56-1.25 1.25v14c0 .69.56 1.25 1.25 1.25h14c.69 0 1.25-.56 1.25-1.25V5c0-.69-.56-1.25-1.25-1.25z",clipRule:"evenodd"})]})}])},394572,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{d:"M8 2.25a.75.75 0 0 1 0 1.5H7c-1.458 0-2.192.253-2.595.655C4.003 4.808 3.75 5.542 3.75 7v10c0 1.459.253 2.192.655 2.595.403.402 1.137.655 2.595.655h9c2.005 0 2.958-.269 3.47-.78.511-.512.78-1.465.78-3.47V7c0-1.458-.253-2.192-.655-2.595-.352-.352-.958-.59-2.081-.643L17 3.75h-1a.75.75 0 0 1 0-1.5h1c1.541 0 2.808.247 3.655 1.095.848.847 1.095 2.113 1.095 3.655v9c0 1.995-.231 3.542-1.22 4.53-.988.989-2.535 1.22-4.53 1.22H7c-1.542 0-2.808-.247-3.655-1.095C2.497 19.808 2.25 18.541 2.25 17V7c0-1.542.247-2.808 1.095-3.655C4.192 2.497 5.458 2.25 7 2.25z"}),(0,t.jsx)("path",{d:"M12 1.25a.75.75 0 0 1 .75.75v11.19l3.72-3.72a.75.75 0 0 1 1.06 1.06l-5 5a.7.7 0 0 1-.16.118q-.041.025-.084.044-.048.02-.098.03-.02.007-.04.012-.007.002-.016.003a.8.8 0 0 1-.265 0l-.016-.003q-.02-.005-.04-.011-.05-.012-.098-.03a.8.8 0 0 1-.243-.163l-5-5a.75.75 0 0 1 1.06-1.06l3.72 3.72V2a.75.75 0 0 1 .75-.75"})]})}])}]);

//# debugId=f73d4907-0f49-6ba7-02c1-8026997011ce
//# sourceMappingURL=21mvq9gh46k_b.js.map