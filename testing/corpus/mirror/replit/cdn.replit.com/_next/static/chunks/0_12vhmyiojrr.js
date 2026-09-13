;!function(){try { var e="undefined"!=typeof globalThis?globalThis:"undefined"!=typeof global?global:"undefined"!=typeof window?window:"undefined"!=typeof self?self:{},n=(new e.Error).stack;n&&((e._debugIds|| (e._debugIds={}))[n]="be72e9ec-d2df-16eb-df74-855dec569352")}catch(e){}}();
(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(["object"==typeof document?document.currentScript:void 0,421642,e=>{"use strict";var t=e.i(274466),r=e.i(230281),i=e.i(640223),n=["formatMatcher","timeZone","hour12","weekday","era","year","month","day","hour","minute","second","timeZoneName","hourCycle","dateStyle","timeStyle","calendar","numberingSystem","fractionalSecondDigits"];function a(e,r,a,s){var o=e.locale,u=e.formats,l=e.onError,c=e.timeZone;void 0===s&&(s={});var m=s.format,d=(0,t.__assign)((0,t.__assign)({},c&&{timeZone:c}),m&&(0,i.getNamedFormat)(u,r,m,l)),g=(0,i.filterProps)(s,n,d);return"time"!==r||g.hour||g.minute||g.second||g.timeStyle||g.dateStyle||(g=(0,t.__assign)((0,t.__assign)({},g),{hour:"numeric",minute:"numeric"})),a(o,g)}e.s(["formatDate",0,function(e,t){for(var i=[],n=2;n<arguments.length;n++)i[n-2]=arguments[n];var s=i[0],o=i[1],u="string"==typeof s?new Date(s||0):s;try{return a(e,"date",t,void 0===o?{}:o).format(u)}catch(t){e.onError(new r.IntlFormatError("Error formatting date.",e.locale,t))}return String(u)},"formatDateTimeRange",0,function(e,t){for(var i=[],n=2;n<arguments.length;n++)i[n-2]=arguments[n];var s=i[0],o=i[1],u=i[2],l="string"==typeof s?new Date(s||0):s,c="string"==typeof o?new Date(o||0):o;try{return a(e,"dateTimeRange",t,void 0===u?{}:u).formatRange(l,c)}catch(t){e.onError(new r.IntlFormatError("Error formatting date time range.",e.locale,t))}return String(l)},"formatDateToParts",0,function(e,t){for(var i=[],n=2;n<arguments.length;n++)i[n-2]=arguments[n];var s=i[0],o=i[1],u="string"==typeof s?new Date(s||0):s;try{return a(e,"date",t,void 0===o?{}:o).formatToParts(u)}catch(t){e.onError(new r.IntlFormatError("Error formatting date.",e.locale,t))}return[]},"formatTime",0,function(e,t){for(var i=[],n=2;n<arguments.length;n++)i[n-2]=arguments[n];var s=i[0],o=i[1],u="string"==typeof s?new Date(s||0):s;try{return a(e,"time",t,void 0===o?{}:o).format(u)}catch(t){e.onError(new r.IntlFormatError("Error formatting time.",e.locale,t))}return String(u)},"formatTimeToParts",0,function(e,t){for(var i=[],n=2;n<arguments.length;n++)i[n-2]=arguments[n];var s=i[0],o=i[1],u="string"==typeof s?new Date(s||0):s;try{return a(e,"time",t,void 0===o?{}:o).formatToParts(u)}catch(t){e.onError(new r.IntlFormatError("Error formatting time.",e.locale,t))}return[]}])},317440,e=>{"use strict";var t=e.i(640223),r=e.i(407147),i=e.i(230281),n=["style","type","fallback","languageDisplay"];e.s(["formatDisplayName",0,function(e,a,s,o){var u=e.locale,l=e.onError;Intl.DisplayNames||l(new r.FormatError('Intl.DisplayNames is not available in this environment.\nTry polyfilling it using "@formatjs/intl-displaynames"\n',r.ErrorCode.MISSING_INTL_API));var c=(0,t.filterProps)(o,n);try{return a(u,c).of(s)}catch(e){l(new i.IntlFormatError("Error formatting display name.",u,e))}}])},27010,e=>{"use strict";var t=e.i(274466),r=e.i(407147),i=e.i(230281),n=e.i(640223),a=["type","style"],s=Date.now();function o(e,o,u,l){var c=e.locale,m=e.onError;void 0===l&&(l={}),Intl.ListFormat||m(new r.FormatError('Intl.ListFormat is not available in this environment.\nTry polyfilling it using "@formatjs/intl-listformat"\n',r.ErrorCode.MISSING_INTL_API));var d=(0,n.filterProps)(l,a);try{var g={},f=Array.from(u).map(function(e,t){if("object"==typeof e&&null!==e){var r="".concat(s,"_").concat(t,"_").concat(s);return g[r]=e,r}return String(e)});return o(c,d).formatToParts(f).map(function(e){return"literal"===e.type?e:(0,t.__assign)((0,t.__assign)({},e),{value:g[e.value]||e.value})})}catch(e){m(new i.IntlFormatError("Error formatting list.",c,e))}return u}e.s(["formatList",0,function(e,t,r,i){void 0===i&&(i={});var n=o(e,t,r,i).reduce(function(e,t){var r=t.value;return"string"!=typeof r?e.push(r):"string"==typeof e[e.length-1]?e[e.length-1]+=r:e.push(r),e},[]);return 1===n.length?n[0]:0===n.length?"":n},"formatListToParts",0,o])},53321,e=>{"use strict";var t=e.i(274466);e.i(254606);var r=e.i(940417),i=e.i(270224),n=e.i(230281),a=e.i(640223);function s(e,r){return Object.keys(e).reduce(function(i,n){return i[n]=(0,t.__assign)({timeZone:r},e[n]),i},{})}function o(e,r){return Object.keys((0,t.__assign)((0,t.__assign)({},e),r)).reduce(function(i,n){return i[n]=(0,t.__assign)((0,t.__assign)({},e[n]||{}),r[n]||{}),i},{})}function u(e,r){if(!r)return e;var n=i.IntlMessageFormat.formats;return(0,t.__assign)((0,t.__assign)((0,t.__assign)({},n),e),{date:o(s(n.date,r),s(e.date||{},r)),time:o(s(n.time,r),s(e.time||{},r))})}e.s(["formatMessage",0,function(e,i,s,o,l){var c=e.locale,m=e.formats,d=e.messages,g=e.defaultLocale,f=e.defaultFormats,p=e.fallbackOnEmptyString,v=e.onError,y=e.timeZone,h=e.defaultRichTextElements;void 0===s&&(s={id:""});var b=s.id,w=s.defaultMessage;(0,a.invariant)(!!b,"[@formatjs/intl] An `id` must be provided to format a message. You can either:\n1. Configure your build toolchain with [babel-plugin-formatjs](https://formatjs.github.io/docs/tooling/babel-plugin)\nor [@formatjs/ts-transformer](https://formatjs.github.io/docs/tooling/ts-transformer) OR\n2. Configure your `eslint` config to include [eslint-plugin-formatjs](https://formatjs.github.io/docs/tooling/linter#enforce-id)\nto autofix this issue");var _=String(b),S=d&&Object.prototype.hasOwnProperty.call(d,_)&&d[_];if(Array.isArray(S)&&1===S.length&&S[0].type===r.TYPE.literal)return S[0].value;if(!o&&S&&"string"==typeof S&&!h)return S.replace(/'\{(.*?)\}'/gi,"{$1}");if(o=(0,t.__assign)((0,t.__assign)({},h),o||{}),m=u(m,y),f=u(f,y),!S){if(!1===p&&""===S)return S;if((!w||c&&c.toLowerCase()!==g.toLowerCase())&&v(new n.MissingTranslationError(s,c)),w)try{var I=i.getMessageFormat(w,g,f,l);return I.format(o)}catch(e){return v(new n.MessageFormatError('Error formatting default message for: "'.concat(_,'", rendering default message verbatim'),c,s,e)),"string"==typeof w?w:_}return _}try{var I=i.getMessageFormat(S,c,m,(0,t.__assign)({formatters:i},l||{}));return I.format(o)}catch(e){v(new n.MessageFormatError('Error formatting message: "'.concat(_,'", using ').concat(w?"default message":"id"," as fallback."),c,s,e))}if(w)try{var I=i.getMessageFormat(w,g,f,l);return I.format(o)}catch(e){v(new n.MessageFormatError('Error formatting the default message for: "'.concat(_,'", rendering message verbatim'),c,s,e))}return"string"==typeof S?S:"string"==typeof w?w:_}])},934537,e=>{"use strict";var t=e.i(230281),r=e.i(640223),i=["style","currency","unit","unitDisplay","useGrouping","minimumIntegerDigits","minimumFractionDigits","maximumFractionDigits","minimumSignificantDigits","maximumSignificantDigits","compactDisplay","currencyDisplay","currencySign","notation","signDisplay","unit","unitDisplay","numberingSystem","trailingZeroDisplay","roundingPriority","roundingIncrement","roundingMode"];function n(e,t,n){var a=e.locale,s=e.formats,o=e.onError;void 0===n&&(n={});var u=n.format,l=u&&(0,r.getNamedFormat)(s,"number",u,o)||{};return t(a,(0,r.filterProps)(n,i,l))}e.s(["formatNumber",0,function(e,r,i,a){void 0===a&&(a={});try{return n(e,r,a).format(i)}catch(r){e.onError(new t.IntlFormatError("Error formatting number.",e.locale,r))}return String(i)},"formatNumberToParts",0,function(e,r,i,a){void 0===a&&(a={});try{return n(e,r,a).formatToParts(i)}catch(r){e.onError(new t.IntlFormatError("Error formatting number.",e.locale,r))}return[]}])},895005,e=>{"use strict";var t=e.i(407147),r=e.i(230281),i=e.i(640223),n=["type"];e.s(["formatPlural",0,function(e,a,s,o){var u=e.locale,l=e.onError;void 0===o&&(o={}),Intl.PluralRules||l(new t.FormatError('Intl.PluralRules is not available in this environment.\nTry polyfilling it using "@formatjs/intl-pluralrules"\n',t.ErrorCode.MISSING_INTL_API));var c=(0,i.filterProps)(o,n);try{return a(u,c).select(s)}catch(e){l(new r.IntlFormatError("Error formatting plural.",u,e))}return"other"}])},489244,e=>{"use strict";var t=e.i(640223),r=e.i(407147),i=e.i(230281),n=["numeric","style"];e.s(["formatRelativeTime",0,function(e,a,s,o,u){void 0===u&&(u={}),o||(o="second"),Intl.RelativeTimeFormat||e.onError(new r.FormatError('Intl.RelativeTimeFormat is not available in this environment.\nTry polyfilling it using "@formatjs/intl-relativetimeformat"\n',r.ErrorCode.MISSING_INTL_API));try{var l,c,m,d,g,f;return(l=u,c=e.locale,m=e.formats,d=e.onError,void 0===l&&(l={}),f=!!(g=l.format)&&(0,t.getNamedFormat)(m,"relative",g,d)||{},a(c,(0,t.filterProps)(l,n,f))).format(s,o)}catch(t){e.onError(new i.IntlFormatError("Error formatting relative time.",e.locale,t))}return String(s)}])},913827,e=>{"use strict";var t=e.i(274466),r=e.i(421642),i=e.i(317440),n=e.i(230281),a=e.i(27010),s=e.i(53321),o=e.i(934537),u=e.i(895005),l=e.i(489244),c=e.i(640223);e.s(["createIntl",0,function(e,m){var d,g=(0,c.createFormatters)(m),f=(0,t.__assign)((0,t.__assign)({},c.DEFAULT_INTL_CONFIG),e),p=f.locale,v=f.defaultLocale,y=f.onError;return p?!Intl.NumberFormat.supportedLocalesOf(p).length&&y?y(new n.MissingDataError('Missing locale data for locale: "'.concat(p,'" in Intl.NumberFormat. Using default locale: "').concat(v,'" as fallback. See https://formatjs.github.io/docs/react-intl#runtime-requirements for more details'))):!Intl.DateTimeFormat.supportedLocalesOf(p).length&&y&&y(new n.MissingDataError('Missing locale data for locale: "'.concat(p,'" in Intl.DateTimeFormat. Using default locale: "').concat(v,'" as fallback. See https://formatjs.github.io/docs/react-intl#runtime-requirements for more details'))):(y&&y(new n.InvalidConfigError('"locale" was not configured, using "'.concat(v,'" as fallback. See https://formatjs.github.io/docs/react-intl/api#intlshape for more details'))),f.locale=f.defaultLocale||"en"),f.onWarn&&f.defaultRichTextElements&&"string"==typeof(d=f.messages||{})[Object.keys(d)[0]]&&f.onWarn('[@formatjs/intl] "defaultRichTextElements" was specified but "message" was not pre-compiled. \nPlease consider using "@formatjs/cli" to pre-compile your messages for performance.\nFor more details see https://formatjs.github.io/docs/getting-started/message-distribution'),(0,t.__assign)((0,t.__assign)({},f),{formatters:g,formatNumber:o.formatNumber.bind(null,f,g.getNumberFormat),formatNumberToParts:o.formatNumberToParts.bind(null,f,g.getNumberFormat),formatRelativeTime:l.formatRelativeTime.bind(null,f,g.getRelativeTimeFormat),formatDate:r.formatDate.bind(null,f,g.getDateTimeFormat),formatDateToParts:r.formatDateToParts.bind(null,f,g.getDateTimeFormat),formatTime:r.formatTime.bind(null,f,g.getDateTimeFormat),formatDateTimeRange:r.formatDateTimeRange.bind(null,f,g.getDateTimeFormat),formatTimeToParts:r.formatTimeToParts.bind(null,f,g.getDateTimeFormat),formatPlural:u.formatPlural.bind(null,f,g.getPluralRules),formatMessage:s.formatMessage.bind(null,f,g),$t:s.formatMessage.bind(null,f,g),formatList:a.formatList.bind(null,f,g.getListFormat),formatListToParts:a.formatListToParts.bind(null,f,g.getListFormat),formatDisplayName:i.formatDisplayName.bind(null,f,g.getDisplayNames)})}])},958422,e=>{"use strict";var t,r,i,n,a=e.i(274466),s=e.i(389959),o=e.i(5620);(t=i||(i={})).formatDate="FormattedDate",t.formatTime="FormattedTime",t.formatNumber="FormattedNumber",t.formatList="FormattedList",t.formatDisplayName="FormattedDisplayName",(r=n||(n={})).formatDate="FormattedDateParts",r.formatTime="FormattedTimeParts",r.formatNumber="FormattedNumberParts",r.formatList="FormattedListParts";var u=function(e){var t=(0,o.default)(),r=e.value,i=e.children,n=(0,a.__rest)(e,["value","children"]);return i(t.formatNumberToParts(r,n))};u.displayName="FormattedNumberParts",u.displayName="FormattedNumberParts",e.s(["createFormattedComponent",0,function(e){var t=function(t){var r=(0,o.default)(),i=t.value,n=t.children,u=(0,a.__rest)(t,["value","children"]),l=r[e](i,u);if("function"==typeof n)return n(l);var c=r.textComponent||s.Fragment;return s.createElement(c,null,l)};return t.displayName=i[e],t},"createFormattedDateTimePartsComponent",0,function(e){var t=function(t){var r=(0,o.default)(),i=t.value,n=t.children,s=(0,a.__rest)(t,["value","children"]),u="string"==typeof i?new Date(i||0):i;return n("formatDate"===e?r.formatDateToParts(u,s):r.formatTimeToParts(u,s))};return t.displayName=n[e],t}])},465725,e=>{"use strict";var t=e.i(274466),r=e.i(913827),i=e.i(53321),n=e.i(487227),a=e.i(627871);function s(e){return e?Object.keys(e).reduce(function(t,r){var i=e[r];return t[r]=(0,n.isFormatXMLElementFn)(i)?(0,a.assignUniqueKeysToParts)(i):i,t},{}):e}var o=function(e,r,n,o){for(var u=[],l=4;l<arguments.length;l++)u[l-4]=arguments[l];var c=s(o),m=i.formatMessage.apply(void 0,(0,t.__spreadArray)([e,r,n,c],u,!1));return Array.isArray(m)?(0,a.toKeyedReactNodeArray)(m):m};e.s(["createIntl",0,function(e,i){var n=e.defaultRichTextElements,u=(0,t.__rest)(e,["defaultRichTextElements"]),l=s(n),c=(0,r.createIntl)((0,t.__assign)((0,t.__assign)((0,t.__assign)({},a.DEFAULT_INTL_CONFIG),u),{defaultRichTextElements:l}),i),m={locale:c.locale,timeZone:c.timeZone,fallbackOnEmptyString:c.fallbackOnEmptyString,formats:c.formats,defaultLocale:c.defaultLocale,defaultFormats:c.defaultFormats,messages:c.messages,onError:c.onError,defaultRichTextElements:l};return(0,t.__assign)((0,t.__assign)({},c),{formatMessage:o.bind(null,m,c.formatters),$t:o.bind(null,m,c.formatters)})}])},549741,e=>{"use strict";var t=e.i(274466),r=e.i(389959),i=e.i(5620),n=function(e){var n=(0,i.default)(),a=e.from,s=e.to,o=e.children,u=(0,t.__rest)(e,["from","to","children"]),l=n.formatDateTimeRange(a,s,u);if("function"==typeof o)return o(l);var c=n.textComponent||r.Fragment;return r.createElement(c,null,l)};n.displayName="FormattedDateTimeRange",e.s(["default",0,n])},514675,e=>{"use strict";var t=e.i(389959),r=e.i(5620),i=function(e){var i=(0,r.default)(),n=i.formatPlural,a=i.textComponent,s=e.value,o=e.other,u=e.children,l=n(s,e),c=e[l]||o;return"function"==typeof u?u(c):a?t.createElement(a,null,c):c};i.displayName="FormattedPlural",e.s(["default",0,i])},411606,e=>{"use strict";var t=e.i(274466),r=e.i(640223),i=e.i(389959),n=e.i(627871),a=e.i(465725),s=e.i(285296);function o(e){return{locale:e.locale,timeZone:e.timeZone,fallbackOnEmptyString:e.fallbackOnEmptyString,formats:e.formats,textComponent:e.textComponent,messages:e.messages,defaultLocale:e.defaultLocale,defaultFormats:e.defaultFormats,onError:e.onError,onWarn:e.onWarn,wrapRichTextChunksInFragment:e.wrapRichTextChunksInFragment,defaultRichTextElements:e.defaultRichTextElements}}var u=function(e){function u(){var t=null!==e&&e.apply(this,arguments)||this;return t.cache=(0,r.createIntlCache)(),t.state={cache:t.cache,intl:(0,a.createIntl)(o(t.props),t.cache),prevConfig:o(t.props)},t}return(0,t.__extends)(u,e),u.getDerivedStateFromProps=function(e,t){var r=t.prevConfig,i=t.cache,s=o(e);return(0,n.shallowEqual)(r,s)?null:{intl:(0,a.createIntl)(s,i),prevConfig:s}},u.prototype.render=function(){return(0,n.invariantIntlContext)(this.state.intl),i.createElement(s.Provider,{value:this.state.intl},this.props.children)},u.displayName="IntlProvider",u.defaultProps=n.DEFAULT_INTL_CONFIG,u}(i.PureComponent);e.s(["default",0,u])},511717,e=>{"use strict";var t=e.i(274466),r=e.i(389959),i=e.i(627871),n=e.i(5620);function a(e){var t=Math.abs(e);return t<60?"second":t<3600?"minute":t<86400?"hour":"day"}function s(e){switch(e){case"second":return 1;case"minute":return 60;case"hour":return 3600;default:return 86400}}var o=["second","minute","hour"];function u(e){return void 0===e&&(e="second"),o.indexOf(e)>-1}var l=function(e){var i=(0,n.default)(),a=i.formatRelativeTime,s=i.textComponent,o=e.children,u=a(e.value||0,e.unit,(0,t.__rest)(e,["children","value","unit"]));return"function"==typeof o?o(u):s?r.createElement(s,null,u):r.createElement(r.Fragment,null,u)},c=function(e){var n,o=e.value,c=void 0===o?0:o,m=e.unit,d=void 0===m?"second":m,g=e.updateIntervalInSeconds,f=(0,t.__rest)(e,["value","unit","updateIntervalInSeconds"]);(0,i.invariant)(!g||!!(g&&u(d)),"Cannot schedule update with unit longer than hour");var p=r.useState(),v=p[0],y=p[1],h=r.useState(0),b=h[0],w=h[1],_=r.useState(0),S=_[0],I=_[1];(d!==v||c!==b)&&(w(c||0),y(d),I(u(d)?function(e,t){if(!e)return 0;switch(t){case"second":return e;case"minute":return 60*e;default:return 3600*e}}(c,d):0)),r.useEffect(function(){function e(){clearTimeout(n)}if(e(),!g||!u(d))return e;var t=S-g,r=a(t);if("day"===r)return e;var i=s(r),o=t%i,l=t-o,c=l>=S?l-i:l,m=Math.abs(c-S);return S!==c&&(n=setTimeout(function(){return I(c)},1e3*m)),e},[S,g,d]);var E=c||0,T=d;if(u(d)&&"number"==typeof S&&g){var C=s(T=a(S));E=Math.round(S/C)}return r.createElement(l,(0,t.__assign)({value:E,unit:T},f))};c.displayName="FormattedRelativeTime",e.s(["default",0,c])},800686,e=>{"use strict";var t=e.i(958422);e.i(465725),e.i(549741),e.i(285296),e.i(847159),e.i(514675),e.i(411606),e.i(511717),e.i(5620),(0,t.createFormattedComponent)("formatDate"),(0,t.createFormattedComponent)("formatTime"),(0,t.createFormattedComponent)("formatNumber"),(0,t.createFormattedComponent)("formatList"),(0,t.createFormattedComponent)("formatDisplayName"),(0,t.createFormattedDateTimePartsComponent)("formatDate"),(0,t.createFormattedDateTimePartsComponent)("formatTime"),e.s(["defineMessages",0,function(e){return e}])},290461,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let i={},n=t.gql`
    query UsePayInvoicesRedirectOrg($orgId: String!) {
  getOrg(orgId: $orgId) {
    __typename
    ... on Org {
      id
      slug
      authorizations {
        payOpenInvoice {
          isAuthorized
        }
      }
    }
  }
}
    `,a=t.gql`
    query UsePayInvoicesRedirectPersonal {
  currentUser {
    __typename
    ... on CurrentUser {
      id
      isRazorpayPaymentProcessor
      openInvoices {
        stripeInvoiceId
      }
    }
  }
}
    `;e.s(["useUsePayInvoicesRedirectOrgQuery",0,function(e){let t={...i,...e};return r.useQuery(n,t)},"useUsePayInvoicesRedirectPersonalQuery",0,function(e){let t={...i,...e};return r.useQuery(a,t)}])},644647,e=>{"use strict";var t=e.i(15801),r=e.i(389959),i=e.i(290461),n=e.i(151027),a=e.i(174474);e.s(["usePayInvoicesRedirect",0,function(e,s){let o=(0,t.useRouter)(),u=(0,n.useCurrentUserStoredOrgContext)(),l=e??u.orgId,c=!!l,m=s?.skip??!1,{data:d,loading:g}=(0,i.useUsePayInvoicesRedirectOrgQuery)({variables:{orgId:l??""},skip:m||!c}),{data:f,loading:p}=(0,i.useUsePayInvoicesRedirectPersonalQuery)({skip:m||c}),v=c?g:p,y=d?.getOrg?.__typename==="Org"?d.getOrg:null,h=y?.authorizations.payOpenInvoice.isAuthorized??!1,b=y?.slug??u.orgSlug,w=f?.currentUser?.__typename==="CurrentUser"?f.currentUser:null,_=(w?.isRazorpayPaymentProcessor??!1)&&(w?.openInvoices?.length??0)>0,S=c?h:_,I=c&&b?`/t/${b}?${(0,a.settingsQueryString)("billing")}`:`/home?${(0,a.settingsQueryString)("billing")}`,E=(0,r.useCallback)(()=>{c&&b?o.push(`/t/${b}?${(0,a.settingsQueryString)("billing")}`):o.push(`/home?${(0,a.settingsQueryString)("billing")}`,`/~?${(0,a.settingsQueryString)("billing")}`)},[c,b,o]);return S?{billingDestination:I,shouldRedirect:!0,isLoading:!1,openBillingTab:E}:{shouldRedirect:!1,isLoading:v}}])},929692,e=>{"use strict";var t=e.i(351623),r=e.i(846545),i=e.i(344480);e.i(975473);var n=e.i(299020);let a={},s=t.gql`
    fragment UsageBasedBillingSuspensionNotification on UsageBasedBillingSuspensionStatusNotification {
  id
  isDismissed
  user {
    id
  }
  customer {
    id
    name
  }
  type
  suspensionScheduledTime
}
    `,o=t.gql`
    subscription UsageBasedBillingSuspensionNotifications($orgId: String) {
  usageBasedBillingSuspensionStatusNotifications(orgId: $orgId) {
    id
    ...UsageBasedBillingSuspensionNotification
  }
}
    ${s}`,u=t.gql`
    query UsageBasedBillingSuspensionNotificationsCurrentUser {
  currentUser {
    id
  }
}
    `,l=t.gql`
    mutation DismissUbbSuspensionNotification($input: DismissUbbSuspensionNotificationInput!) {
  dismissUbbSuspensionNotification(input: $input) {
    ... on UsageBasedBillingSuspensionStatusNotification {
      ...UsageBasedBillingSuspensionNotification
    }
  }
}
    ${s}`;e.s(["useDismissUbbSuspensionNotificationMutation",0,function(e){let t={...a,...e};return n.useMutation(l,t)},"useUsageBasedBillingSuspensionNotificationsCurrentUserQuery",0,function(e){let t={...a,...e};return i.useQuery(u,t)},"useUsageBasedBillingSuspensionNotificationsSubscription",0,function(e){let t={...a,...e};return r.useSubscription(o,t)}])},658862,e=>{"use strict";var t=e.i(389959),r=e.i(929692),i=e.i(151027);e.s(["useUBBSuspensionNotifications",0,function({skip:e=!1,overrideOrgId:n}={}){let{orgId:a}=(0,i.useCurrentUserStoredOrgContext)(),{data:s,loading:o}=(0,r.useUsageBasedBillingSuspensionNotificationsCurrentUserQuery)({skip:e}),{data:u,loading:l,error:c}=(0,r.useUsageBasedBillingSuspensionNotificationsSubscription)({variables:{orgId:n??a??void 0},skip:e||s?.currentUser?.__typename!=="CurrentUser"||o}),[m]=(0,r.useDismissUbbSuspensionNotificationMutation)(),[d,g]=(0,t.useState)(null);return{suspensionNotification:u?.usageBasedBillingSuspensionStatusNotifications?.__typename==="UsageBasedBillingSuspensionStatusNotification"&&d!==u.usageBasedBillingSuspensionStatusNotifications.id?u.usageBasedBillingSuspensionStatusNotifications:null,suspensionNotificationsLoading:l,suspensionNotificationsError:c,handleDismissNotification:e=>{g(e),m({variables:{input:{notificationId:e}}})}}}])},476601,e=>{"use strict";var t=e.i(908796),r=e.i(761201);let i=["/home","/cycles","/usage","/account","/bounties","/templates","/learn","/repls","/replEnvironmentDesktop","/replEnvironmentMobile","/chatMobile","/new","/profile","/my-teams"];e.s(["convertDecimalToDisplayPercent",0,function(e){if(e>1)return"100%+";let t=(100*e).toLocaleString(void 0,{maximumFractionDigits:0});return`${t}%`},"getUbbSuspensionDescription",0,e=>{let i,n="",a=e.isRazorpay;switch(e.type){case"org":if(e.orgDealContext?.dealType===t.OrgDealType.Trial||e.orgDealContext?.dealType===t.OrgDealType.EnterpriseTrial){i=e.isAdmin?`Your Replit trial has ended. Contact ${e.orgDealContext.salesContactEmail??r.SALES_TEAM_CONTACT_EMAIL} to resume services.`:"Your Replit trial has ended. Contact your admin to resume services.";break}i=e.isSuspended?e.isAdmin?a?"Your workspace has been temporarily suspended due to a failed payment. Please pay your outstanding invoices to continue using Agent.":"Your workspace has been temporarily suspended due to a failed payment. Please update your workspace's payment method and address unpaid invoices to continue using Agent.":"Your workspace has been temporarily suspended due to a failed payment. Contact your admin to continue using Agent.":e.isAdmin?a?"Your workspace has a failed payment. Please pay your outstanding invoices to continue using Agent.":"Your workspace has a failed payment. Please update your workspace's payment method to continue using Agent.":"Your workspace has a failed payment. Contact your admin to continue using Agent.",n=e.isAdmin?a?"If you have already paid your invoices, please wait a few minutes and refresh the page.":"If you have already updated your workspace's payment method, please wait a few minutes and refresh the page.":a?"If your admin has already paid the outstanding invoices, please wait a few minutes and refresh the page.":"If your admin has already updated your workspace's payment method, please wait a few minutes and refresh the page.";break;case"user":i=e.isSuspended?"Your account has been temporarily suspended due to a failed payment":a?"You have a failed payment. Please pay your outstanding invoices to continue using Agent.":"You have a failed payment. Please update your payment method to continue using Agent.",n=a?"If you have already paid your invoices, please wait a few minutes and refresh the page.":"If you have already updated your payment method, please wait a few minutes and refresh the page."}return{headingText:i,subheadingText:n}},"shouldShowUsageAlert",0,e=>i.includes(e)||e.startsWith("/t/[orgSlug]")])},61041,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);let i={},n=t.gql`
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
    `,s=t.gql`
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
    `,o=t.gql`
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
    ${s}
${a}
${n}`,u=t.gql`
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
    `,l=t.gql`
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
    `,m=t.gql`
    query UniversalSettingsNavigationOrgReference($orgId: String!) {
  getOrg(orgId: $orgId) {
    __typename
    ... on Org {
      id
    }
  }
}
    `;e.s(["UniversalSettingsNavigationCurrentUserCustomerReferenceDocument",0,c,"UniversalSettingsNavigationCustomerReferenceDocument",0,l,"UniversalSettingsNavigationOrgReferenceDocument",0,m,"usePrefetchUniversalSettingsDedicatedAccessQuery",0,function(e){let t={...i,...e};return r.useQuery(u,t)},"usePrefetchUniversalSettingsNavigationQuery",0,function(e){let t={...i,...e};return r.useQuery(o,t)}])},587719,e=>{"use strict";var t=e.i(389959),r=e.i(61041);e.s(["usePrefetchUniversalSettingsNavigation",0,function(e){let i="page"===e.type?e.scope:void 0,n=i?.type==="org"?i.orgId:void 0,a=i?.type==="org"?i.orgSlug:void 0,[s,o]=(0,t.useState)(null),u=(0,t.useRef)(null),l=null;i?.type==="org"?l=`org:${i.orgId}`:i?.type==="personal"&&(l="personal");let c=void 0!==n&&void 0!==a,m=null!==l&&s!==l;(0,t.useEffect)(()=>{if(!m)return;let e=()=>o(l);if("function"==typeof window.requestIdleCallback){let t=window.requestIdleCallback(e,{timeout:2e3});return()=>window.cancelIdleCallback(t)}let t=window.setTimeout(e,0);return()=>window.clearTimeout(t)},[l,m]);let d=null!==l&&s===l,{data:g,client:f}=(0,r.usePrefetchUniversalSettingsNavigationQuery)({variables:{orgId:n??"",orgSlug:a??"",includeOrg:c},skip:!d,ssr:!1,fetchPolicy:"cache-first"}),p=g?.currentUser?.__typename==="CurrentUser"?g.currentUser:void 0,v=c&&g?.getOrg?.__typename==="Org"&&g.getOrg.id===n&&g.getOrg.slug===a?g.getOrg:void 0,y=v?.customer?.__typename==="Customer"?v.customer:void 0,h=p?.existingCustomer?.__typename==="Customer"?p.existingCustomer:void 0,b=c?y:h,w=d&&b?.authorizations.editSettings.isAuthorized===!1;(0,r.usePrefetchUniversalSettingsDedicatedAccessQuery)({variables:{customerId:b?.id??0,includeApprovalRequests:c},skip:!w,ssr:!1,fetchPolicy:"cache-first"}),(0,t.useEffect)(()=>{d&&void 0!==g&&u.current!==l&&void 0!==b&&(f.cache.writeQuery({query:r.UniversalSettingsNavigationCustomerReferenceDocument,variables:{customerId:b.id},data:{getCustomer:{__typename:"Customer",id:b.id}}}),c||void 0===p||f.cache.writeQuery({query:r.UniversalSettingsNavigationCurrentUserCustomerReferenceDocument,data:{currentUser:{__typename:"CurrentUser",id:p.id,customer:{__typename:"Customer",id:b.id}}}}),void 0!==v&&f.cache.writeQuery({query:r.UniversalSettingsNavigationOrgReferenceDocument,variables:{orgId:v.id},data:{getOrg:{__typename:"Org",id:v.id}}}),u.current=l)},[f.cache,p,b,l,g,c,v,d])}])},755562,e=>{"use strict";var t=e.i(500396),r=e.i(309094);let i=(0,t.makeVar)({kind:"pending"});e.s(["setSiteControlPulseState",0,function(e){i(e)},"useSiteControlPulseState",0,function(){return(0,r.useReactiveVar)(i)}])},473833,e=>{"use strict";var t=e.i(526687),r=e.i(68701);e.s(["default",0,function(){let e=(0,r.useUserAgent)();return(0,t.desktopAppUserAgentHostsAtLeast)(e,t.DESKTOP_TAB_BAR_MIN_VERSION)},"useDesktopAppHostsRealHome",0,function(){let e=(0,r.useUserAgent)();return(0,t.desktopAppUserAgentHostsAtLeast)(e,t.DESKTOP_REAL_HOME_MIN_VERSION)}])},196178,e=>{"use strict";var t=e.i(389959);e.s(["useEffectOnce",0,function(e){let r=(0,t.useRef)(!1);(0,t.useEffect)(()=>{r.current||(r.current=!0,e())},[e])}])},233763,e=>{"use strict";var t=e.i(389959),r=e.i(830675),i=e.i(320216),n=e.i(871752),a=e.i(489859);let s="email-verification-resend-timestamp";e.s(["useEmailVerificationResend",0,function(){let[e,o]=(0,t.useState)(!1),[u,l]=(0,t.useState)(0),{showConfirm:c,showError:m}=(0,i.default)();return(0,t.useEffect)(()=>{try{let e=a.default.get(s,"number");if(e){let t=Date.now()-e;t<6e4?(o(!0),l(Math.ceil((6e4-t)/1e3))):a.default.remove(s)}}catch(e){r.captureException(e)}},[]),(0,t.useEffect)(()=>{if(!e||u<=0)return;let t=setInterval(()=>{l(e=>{let i=e-1;if(i<=0){o(!1);try{a.default.remove(s)}catch(e){r.captureException(e)}return clearInterval(t),0}return i})},1e3);return()=>clearInterval(t)},[e,u]),{resendVerification:(0,t.useCallback)(async()=>{if(!e)try{await (0,n.postJson)("/data/user/resend_verification",{}),c("Verification email sent"),o(!0),l(60);try{a.default.set(s,Date.now())}catch(e){r.captureException(e)}}catch(t){let{message:e}=t;m(`Failed to resend verification email: ${e}`),r.captureException(t)}},[e,c,m]),isInCooldown:e,cooldownTimeRemaining:u}}])},730497,e=>{"use strict";var t=e.i(389959),r=e.i(775973),i=e.i(415541);let n="__REPLIT__USER_FLAGS__",a=(0,t.createContext)(null),s=a.Provider;function o(){return(0,t.useContext)(a)}e.s(["FlagsProvider",0,s,"USER_FLAGS_KEY",0,n,"getUserFlags",0,function(e){return e?e.user?.id?{flags:e.user.gating?Object.fromEntries(e.user.gating.map(e=>[e.controlName,e])):{},userId:e.user.id}:null:window[n]??null},"useFlag",0,function(e){let t,{controlName:n,default:a}=e,{type:s,logFlagToAnalytics:u,forceOnlyAnonymous:l}=e;s=s??"boolean";let c=(0,r.useLDClient)(),m=o(),d=!(m?.flags&&m?.userId)||l&&!0;if(t=d?(({controlName:e,type:t="boolean",fallbackValue:r,ldClient:i})=>{let n=i?.variation(e,void 0);if(void 0===n){if(void 0!==r)return r;if("boolean"===t)return!1;throw Error(`${e} Did you forget to specify the default value?`)}if(typeof n!==t)throw Error(`Gate type expected to be ${t}. Did you forget to specify the type?`);return n})({controlName:n,fallbackValue:a,type:s,ldClient:c}):(({controlName:e,type:t="boolean",fallbackValue:r,userFlags:i})=>{let n=i.flags[e];if(!n){if(void 0!==r)return r;if("boolean"===t)return!1;throw Error("Control not found")}if("boolean"===t){if(!n.type||"boolean"===n.type)return n.value;throw Error("Gate type expected to be boolean. Did you forget to specify the type?")}if("string"===t){if("string"===n.type||"multivariate"===n.type)return n.value;throw Error("Gate type expected to be string. Did you forget to specify the type?")}if("number"===t){if("number"===n.type)return n.value;if("multivariate"===n.type)return Number(n.value);throw Error("Gate type expected to be number. Did you forget to specify the type?")}throw Error("Unsupported gate type.")})({controlName:n,fallbackValue:a,type:s,userFlags:m}),!u)return t;let g={gating:[{value:t,type:s,controlName:n}]};return d&&(0,i.anonIdentify)(g),t},"useUserFlags",0,o])},547523,e=>{"use strict";var t=e.i(389959),r=e.i(349892);let i={mobileMax:"max",mobileMin:"min",tabletMax:"max",tabletMin:"min"};function n(e){let[r,i]=(0,t.useState)(window.matchMedia(e).matches);function n(){i(window.matchMedia(e).matches)}return(0,t.useEffect)(()=>{let t=window.matchMedia(e);return n(),t.addListener?t.addListener(n):t.addEventListener("change",n),()=>{t.removeListener?t.removeListener(n):t.removeEventListener("change",n)}},[e]),r}e.s(["useBreakpoint",0,function(e){let t=i[e];return n(`(${t}-width: ${r.BREAKPOINTS[e]}px)`)},"useMediaQuery",0,n])},955410,e=>{"use strict";var t=e.i(389959),r=e.i(196178),i=e.i(753451),n=e.i(415541),a=e.i(709485),s=e.i(926684);function o(){let e=(0,s.useTrackingHierarchy)(),r=(0,i.useIsInBonsaiWebview)();return{trackImpression:(0,t.useCallback)(t=>{let i={...e,...t,isBonsai:r};(0,n.track)(a.events.IMPRESSION,i)},[e,r])}}e.s(["useTrackClick",0,function(){let e=(0,s.useTrackingHierarchy)(),r=(0,i.useIsInBonsaiWebview)();return{trackClick:(0,t.useCallback)(t=>{let i=(0,n.getTrackV2PlanType)()??void 0,s={...e,...t,isBonsai:r,...i?{userPlanName:i}:{}};(0,n.track)(a.events.CLICK,s)},[e,r])}},"useTrackImpression",0,o,"useTrackImpressionOnce",0,function(e){let{trackImpression:t}=o();(0,r.useEffectOnce)(()=>{t(e)})}])},505690,e=>{"use strict";var t=e.i(722045),r=e.i(489859);let i=/%(?:0[0-9a-f]|1[0-9a-f]|7f)/gi;e.s(["default",0,class{static get(e){return r.default.get(e,"string")||t.default.get(e)}static set(e,n){let a=function(e){let t="";for(let r of e){let e=r.charCodeAt(0);e<=31||127===e||(t+=r)}return t.replace(i,"").slice(0,1024)}(n);r.default.set(e,a),t.default.set(e,a)}static remove(e){r.default.remove(e),t.default.remove(e)}}])},497840,e=>{"use strict";var t=e.i(351623),r=e.i(299020);let i={},n=t.gql`
    mutation CookieConsentUpdateTrackingConsent($input: UpdateUserPrivacyPreferencesInput!) {
  updateUserPrivacyPreferences(input: $input) {
    ... on CurrentUser {
      id
      trackingConsent
    }
    ... on UnauthorizedError {
      message
    }
    ... on UserError {
      message
    }
  }
}
    `;e.s(["useCookieConsentUpdateTrackingConsentMutation",0,function(e){let t={...i,...e};return r.useMutation(n,t)}])},124389,e=>{"use strict";var t=e.i(389959),r=e.i(497840),i=e.i(570438);e.s(["usePersistTrackingConsent",0,function(){let{currentUserId:e,isCurrentUserIdLoading:n}=(0,i.useCurrentUserIdState)({skip:!1}),[a]=(0,r.useCookieConsentUpdateTrackingConsentMutation)();return(0,t.useCallback)(async t=>{if(!e&&!n)return!1;try{let{data:e}=await a({variables:{input:{tracking_consent:t}}});return e?.updateUserPrivacyPreferences.__typename==="CurrentUser"}catch{return!1}},[e,n,a])}])},279849,e=>{"use strict";let t;var r=e.i(722045),i=e.i(775973),n=e.i(912206),a=e.i(365669),s=e.i(505690),o=e.i(415541);let u="gating_id",l="ld_uid",c={clientSideID:a.publicEnv.NEXT_PUBLIC_LAUNCH_DARKLY_CLIENT_ID||"",context:{key:s.default.get(l)||(window?.ldAnonId?t=window.ldAnonId:((t=(0,o.getAnonymousId)()||r.default.get(u))||(t=(0,n.v4)()),(0,o.getAnonymousId)()!==t&&(0,o.setAnonymousId)(t),window.ldAnonId=t),r.default.get(u)||r.default.set(u,t,{path:"/"}),t),kind:"user",anonymous:!s.default.get(l)},options:{bootstrap:"localStorage",sendEvents:!0,streaming:!1,fetchGoals:!1},reactOptions:{useCamelCaseFlagKeys:!1}};e.s(["default",0,()=>(0,i.withLDProvider)(c),"setLdUserId",0,e=>{s.default.set(l,e)}])},871752,e=>{"use strict";var t=e.i(324753),r=e.i(272391);function i(e,r){return(0,t.default)(e,{credentials:"same-origin",headers:{"Content-Type":"application/json",Accept:"application/json","X-Requested-With":"XMLHttpRequest"},method:"post",body:JSON.stringify(r)})}e.s(["postJson",0,function(e,t={}){var n,a;let s;return n=i(e,t),a=e,s=new r.default("Unknown http error"),Promise.resolve(n).then(async e=>{let t;if(e.ok)return e.json();let r=e.headers.get("content-type");if(r&&r.includes("application/json"))t=await e.json();else{let r=await e.text();try{t=JSON.parse(r)}catch(e){t={message:r}}}throw t.message&&(s.message=t.message),s.setExtras({url:a,responseBody:t,responseData:{status:e.status,statusText:e.statusText,redirected:e.redirected,type:e.type,url:e.url}}).setTag("httpError","true"),s})},"wrapPost",0,i])},21419,e=>{"use strict";var t=e.i(389959);async function r(){if(!("u"<typeof document)&&document.hidden)return new Promise(e=>{let t=()=>{document.hidden||(document.removeEventListener("visibilitychange",t),e())};document.addEventListener("visibilitychange",t)})}e.s(["pageVisible",0,r,"usePageVisibility",0,function(){let[e,r]=(0,t.useState)(!1);return(0,t.useEffect)(()=>{function e(){r(!document.hidden)}return document.addEventListener("visibilitychange",e),e(),()=>{document.removeEventListener("visibilitychange",e)}},[]),e},"useRefetchOnPageVisible",0,function(e){let r=(0,t.useRef)(e);r.current=e,(0,t.useEffect)(()=>{function e(){document.hidden||r.current()}return document.addEventListener("visibilitychange",e),()=>{document.removeEventListener("visibilitychange",e)}},[])}])},124298,e=>{"use strict";var t=e.i(276385),r=e.i(602686),i=e.i(983420);e.i(459890);var n=e.i(500355),a=e.i(547523);e.i(214847);var s=e.i(864300),o=e.i(27923),u=e.i(919073),l=e.i(643484),c=e.i(419635),m=e.i(488299),d=e.i(8047),g=e.i(61732);e.s(["TopBanner",0,({buttonAnalyticsId:e,buttonAnalyticsDestination:f,dataAnalyticsId:p,dismissButtonAnalyticsId:v,innerRef:y,text:h,iconLeft:b,buttonProps:w,buttonLinkProps:_,colorway:S,onDismiss:I,mobileLayout:E="stacked",children:T})=>{let C=(0,s.useIntl)(),P=(0,a.useBreakpoint)("mobileMax"),U=P&&"stacked"===E,N=P?"small":"default",A=null;w?A=(0,t.jsx)(l.Button,{"data-analytics-id":e,"data-analytics-destination":f,...w,size:w.size??N}):_&&(A=(0,t.jsx)(c.ButtonLink,{"data-analytics-id":e,"data-analytics-destination":f,..._,size:_.size??N}));let x=I?(0,t.jsx)(m.IconButton,{colorway:S,"data-analytics-id":v,alt:C.formatMessage({id:"rui.topBannerDismiss",defaultMessage:"Dismiss"}),onClick:I,children:(0,t.jsx)(r.default,{})}):null;return(0,t.jsx)(u.ShadesSurface,{"data-analytics-id":p,border:P?{side:["top","bottom"],strength:"subtle"}:"subtle",colorShade:S,clsx:(0,o.tw)("shrink justify-center rounded-full py-100 px-300",o.tw.on("max-mobile-max")("grow rounded-none py-100 px-150")),elevate:"1x",innerRef:y,children:(0,t.jsxs)(g.View,{clsx:o.tw.merge("relative flex flex-row items-center justify-between gap-200 w-full min-w-0",o.tw.external(n.classes.text)),children:[(0,t.jsxs)(g.View,{clsx:(0,o.tw)("flex flex-1 flex-row items-center justify-center gap-200 min-w-0","row"===E?o.tw.on("max-mobile-max")("flex-row gap-200 py-0 px-50"):o.tw.on("max-mobile-max")("flex-col gap-100 py-50 px-200")),children:[(0,t.jsxs)(g.View,{clsx:(0,o.tw)("flex flex-row grow shrink items-center","row"===E?"justify-start gap-200":"justify-center gap-100"),children:[b&&!U?(0,t.jsx)(g.View,{clsx:(0,o.tw)("shrink-0"),children:(0,t.jsx)(i.IconProvider,{size:16,children:b})}):null,(0,t.jsx)(d.Text,{multiline:U,showTooltipOnTruncate:!U,textAlign:U?"center":void 0,textWrap:U?"balance":void 0,shrink:!0,children:h})]}),T,A?(0,t.jsx)(g.View,{clsx:(0,o.tw)("flex flex-row shrink-0 items-center gap-100"),children:A}):null]}),x?(0,t.jsx)(g.View,{clsx:(0,o.tw)("shrink-0",o.tw.on("max-mobile-max")((0,o.tw)("top-0 right-0","row"===E?"static":"absolute"))),children:x}):null]})})}])},691565,e=>{"use strict";var t=e.i(351623),r=e.i(344480);e.i(975473);var i=e.i(299020);let n={},a=t.gql`
    query ThemePreferenceCurrentUser {
  currentUser {
    id
    workspacePreferences
  }
}
    `,s=t.gql`
    mutation ThemePreferenceUpdate($input: JSON!) {
  updateWorkspacePreferences(input: $input) {
    id
    workspacePreferences
  }
}
    `;e.s(["useThemePreferenceCurrentUserQuery",0,function(e){let t={...n,...e};return r.useQuery(a,t)},"useThemePreferenceUpdateMutation",0,function(e){let t={...n,...e};return i.useMutation(s,t)}])},841114,e=>{"use strict";var t=e.i(691565),r=e.i(320216);e.s(["useThemePreference",0,function(){let{showError:e}=(0,r.default)(),{data:i}=(0,t.useThemePreferenceCurrentUserQuery)({ssr:!0}),n=i?.currentUser?.__typename==="CurrentUser"?i.currentUser:null,a=n?.workspacePreferences.theme,s=null==a,[o]=(0,t.useThemePreferenceUpdateMutation)(),u=async t=>{if(!n)return;let r="system"===t?null:t;null===r&&s||(r!==a||s)&&await o({variables:{input:{theme:r}},optimisticResponse:{__typename:"RootMutationType",updateWorkspacePreferences:{__typename:"CurrentUser",id:n.id,workspacePreferences:{...n.workspacePreferences,theme:r}}},onError:()=>e("Something went wrong setting the active theme - please try again.")})};return{isSystemTheme:s,setActiveTheme:u}}])},443505,e=>{"use strict";var t=e.i(389959),r=e.i(753451),i=e.i(584878);e.s(["default",0,function({onChange:e}){let n=(0,r.useIsInBonsaiWebview)();(0,t.useEffect)(()=>{if(!n)return;let t=t=>(0,i.payingStatusChangedBridgeMessageHandler)(t,()=>{e()});return window.addEventListener("message",t),()=>{window.removeEventListener("message",t)}},[n,e])}])},638141,e=>{"use strict";var t=e.i(15801),r=e.i(389959),i=e.i(179104),n=e.i(582168),a=e.i(753451),s=e.i(584878);e.s(["default",0,function(){let e=(0,t.useRouter)(),o=(0,a.doesBonsaiWebviewSupportFeature)(e,"stripePayment")&&(0,a.isInBonsaiWebview)(e);return{showPaymentFlow:(0,r.useCallback)(e=>{let t={messageType:n.BridgeMessageType.SHOW_PAYMENT_FLOW,flow:e};switch(e.type){case"setup":if(!o)break;(0,s.sendMessage)(t,(0,i.nanoid)());break;case"setUsageLimits":(0,s.sendMessage)(t,(0,i.nanoid)())}},[o])}}])},519979,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsx)(r.default,{...e,children:(0,t.jsx)("path",{d:"M9 1.25a1 1 0 0 1 .902.57l.058.15.002.008L15 19.9l1.868-6.643a2.75 2.75 0 0 1 2.652-2.007H22a.75.75 0 0 1 0 1.5h-2.481a1.25 1.25 0 0 0-1.206.912l-2.351 8.361-.002.007a1.001 1.001 0 0 1-1.92 0l-.002-.008L9 4.1l-1.867 6.644a2.75 2.75 0 0 1-2.64 2.007H2a.75.75 0 0 1 0-1.5h2.488a1.25 1.25 0 0 0 1.2-.913l2.35-8.36.002-.007a1 1 0 0 1 .36-.52l.136-.086A1 1 0 0 1 9 1.25"})})}])},393428,e=>{"use strict";var t=e.i(276385),r=e.i(983420);e.s(["default",0,function(e){return(0,t.jsxs)(r.default,{...e,children:[(0,t.jsx)("path",{d:"M6.5 11.25a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5M17.5 9.25a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5M8.5 6.25a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5M13.5 5.25a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5"}),(0,t.jsx)("path",{fillRule:"evenodd",d:"M12 1.25c2.827 0 5.553 1.01 7.573 2.828C21.596 5.9 22.75 8.388 22.75 11A5.75 5.75 0 0 1 17 16.75h-2.25a1 1 0 0 0-.8 1.6l.3.4.1.143a2.5 2.5 0 0 1-2.1 3.857H12a10.75 10.75 0 1 1 0-21.5m0 1.5a9.25 9.25 0 1 0 0 18.5h.25a1 1 0 0 0 .8-1.6l-.3-.4a2.5 2.5 0 0 1 2-4H17l.21-.005A4.25 4.25 0 0 0 21.25 11c0-2.161-.953-4.252-2.68-5.807C16.84 3.636 14.476 2.75 12 2.75",clipRule:"evenodd"})]})}])}]);

//# debugId=be72e9ec-d2df-16eb-df74-855dec569352
//# sourceMappingURL=1s15rx6l_oo20.js.map