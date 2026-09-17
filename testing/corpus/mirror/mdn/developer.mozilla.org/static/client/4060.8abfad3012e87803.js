export const __rspack_esm_id=4060;export const __rspack_esm_ids=[4060];export const __webpack_modules__={85952(t,o,a){var e=a(22009),r=a(31601),l=a.n(r),n=a(76314),c=(a.n(n))()(l());c.push([t.id,'.bottom-placement{--bottom-background-light:var(--color-background-primary);--bottom-color-light:var(--color-text-primary);--bottom-background-dark:var(--color-background-primary);--bottom-color-dark:var(--color-text-primary);--csstools-light-dark-toggle-9459a664-0:var(--csstools-color-scheme--light) var(--bottom-background-dark);--bottom-background:var(--csstools-light-dark-toggle-9459a664-0,var(--bottom-background-light));--csstools-light-dark-toggle-9459a664-1:var(--csstools-color-scheme--light) var(--bottom-color-dark);--bottom-color:var(--csstools-light-dark-toggle-9459a664-1,var(--bottom-color-light));background-color:var(--bottom-background);padding-inline:var(--layout-side-padding)}@supports (color:light-dark(red,red)){.bottom-placement{--bottom-background:light-dark(var(--bottom-background-light),var(--bottom-background-dark));--bottom-color:light-dark(var(--bottom-color-light),var(--bottom-color-dark))}}@supports not (color:light-dark(tan,tan)){.bottom-placement *{--csstools-light-dark-toggle-9459a664-0:var(--csstools-color-scheme--light) var(--bottom-background-dark);--bottom-background:var(--csstools-light-dark-toggle-9459a664-0,var(--bottom-background-light));--csstools-light-dark-toggle-9459a664-1:var(--csstools-color-scheme--light) var(--bottom-color-dark);--bottom-color:var(--csstools-light-dark-toggle-9459a664-1,var(--bottom-color-light))}}.bottom-placement .placement-container{column-gap:3rem;display:grid;grid-template-areas:"nope pong note" "nope pong no";grid-template-columns:auto auto auto;margin:0 auto;padding:0 1rem}@media (width <= 1441px){.bottom-placement .placement-container{grid-template-areas:"pong note" "pong no";grid-template-columns:auto max-content;grid-template-rows:auto 2rem}}@media (width <= 769px){.bottom-placement .placement-container{grid-template-areas:"pong" "note";grid-template-columns:auto;grid-template-rows:auto 2rem}}:is(.bottom-placement .placement-container) .placement-link{display:flex;grid-area:pong;justify-content:center}:is(:is(.bottom-placement .placement-container) .placement-link) img{display:block;height:auto;max-width:100%}:is(.bottom-placement .placement-container) mdn-placement-note{grid-area:note;margin:0 0 auto auto}',""]);let m=(0,e.AH)([c.toString()]);a.d(o,{},{A:m})},7895(t,o,a){a.r(o);var e=a(22009),r=a(6616),l=a(84199);a(47764),a(13755);var n=a(51360),c=a(85952);let MDNPlacementBottom=class MDNPlacementBottom extends(0,n.N)(e.WF){static styles=c.A;renderComplete(t){let o=t?.hpFooter||t?.bottom;if(!o)return e.s6;let{status:a,click:n,view:c,image:m,alt:i,colors:s,version:g}=o;if("success"!==a)return e.s6;this._viewedUrl||(this._viewedUrl=c,this._version=g);let{backgroundColor:d,textColor:p}=s||{},b=Object.fromEntries([["--bottom-background",d],["--bottom-color",p]].filter(([t,o])=>o)),h=t?.hpFooter?"hp-footer":"bottom-banner";return(0,e.qy)`<div
      ${(0,r.K)(this._placementRef)}
      class="bottom-placement"
      style=${(0,l.W)(b)}
      data-type=${h}
    >
      <section class="placement-container">
        <a
          class="placement-link"
          data-glean-id=${`pong: pong->click ${h}`}
          href=${this.clickLink(n,g)}
          target="_blank"
          rel="sponsored"
        >
          <img
            src=${this.imgLink(m)}
            aria-hidden=${!i}
            alt=${i||""}
            width="728"
            height="90"
          />
        </a>
        <mdn-placement-note></mdn-placement-note>
      </section>
    </div>`}};customElements.define("mdn-placement-bottom",MDNPlacementBottom),a.d(o,{MDNPlacementBottom:()=>MDNPlacementBottom})}};
//# sourceMappingURL=4060.8abfad3012e87803.js.map