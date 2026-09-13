import{i as e}from"./rolldown-runtime-aKtaBQYM.js";import{n as t}from"./@mux-Ce8MRpmL.js";import{ld as n}from"./vendor-B3BotZjU.js";import{t as r}from"./cn-C-UZ2Rd-.js";import{t as i}from"./tooltip-BxatTfEj.js";import{t as a}from"./IconDot-fDReC_7n.js";var o=e(t(),1),s=n(),c={green:`text-content-green`,brand:`text-content-brand`,orange:`text-content-orange`,red:`text-content-red`,gray:`text-content-tertiary`},l=({size:e,color:t,variant:n=`solid`,animation:r=`none`,tooltip:a,className:o})=>{let l=c[t],u=(0,s.jsx)(d,{size:e,colorClass:l,variant:n,animation:r,tooltip:a,className:o});return a?(0,s.jsx)(i,{content:a,usePortal:!0,children:(0,s.jsx)(`span`,{className:`inline-flex align-middle`,children:u})}):u},u={position:`absolute`,inset:0},d=({size:e,colorClass:t,variant:n,animation:i,tooltip:o,className:c})=>i!==`none`&&e===`lg`?i===`spin`?(0,s.jsxs)(`div`,{"aria-label":o,"data-testid":`status-dot`,className:r(`relative block`,t,c),style:{width:24,height:24},children:[(0,s.jsx)(`style`,{children:`
                            @keyframes spin-ring {
                                0% {
                                    transform: rotate(0deg);
                                }
                                100% {
                                    transform: rotate(360deg);
                                }
                            }
                            .spin-ring {
                                transform-origin: center;
                                animation: spin-ring 2s linear infinite;
                                will-change: transform;
                            }
                        `}),(0,s.jsx)(`svg`,{width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`3.5`,fill:`currentColor`,stroke:`currentColor`,strokeOpacity:`0.15`,strokeWidth:`6`})}),(0,s.jsx)(`svg`,{className:`spin-ring`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`6`,fill:`none`,stroke:`currentColor`,strokeOpacity:`0.5`,strokeWidth:`1`,strokeLinecap:`round`,strokeDasharray:`7 31`})})]}):i===`spin-reverse`?(0,s.jsxs)(`div`,{"aria-label":o,"data-testid":`status-dot`,className:r(`relative block`,t,c),style:{width:24,height:24},children:[(0,s.jsx)(`style`,{children:`
                            @keyframes spin-ring-reverse {
                                0% {
                                    transform: rotate(360deg);
                                }
                                100% {
                                    transform: rotate(0deg);
                                }
                            }
                            .spin-ring-reverse {
                                transform-origin: center;
                                animation: spin-ring-reverse 2s linear infinite;
                                will-change: transform;
                            }
                        `}),(0,s.jsx)(`svg`,{width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`3.5`,fill:`currentColor`,stroke:`currentColor`,strokeOpacity:`0.15`,strokeWidth:`6`})}),(0,s.jsx)(`svg`,{className:`spin-ring-reverse`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`6`,fill:`none`,stroke:`currentColor`,strokeOpacity:`0.5`,strokeWidth:`1`,strokeLinecap:`round`,strokeDasharray:`7 31`})})]}):i===`fade`?(0,s.jsxs)(`div`,{"aria-label":o,"data-testid":`status-dot`,className:r(`relative block`,t,c),style:{width:24,height:24},children:[(0,s.jsx)(`style`,{children:`
                            @keyframes fade-pulse {
                                0%, 100% {
                                    opacity: 0.9;
                                }
                                50% {
                                    opacity: 0.5;
                                }
                            }
                            .fade-dot {
                                animation: fade-pulse 2.5s ease-in-out infinite;
                                will-change: opacity;
                            }
                        `}),(0,s.jsx)(`svg`,{className:`fade-dot`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})})]}):i===`heartbeat`?(0,s.jsxs)(`div`,{"aria-label":o,"data-testid":`status-dot`,className:r(`relative block`,t,c),style:{width:24,height:24},children:[(0,s.jsx)(`style`,{children:`
                            @keyframes hb-pulse {
                                0% {
                                    transform: scale(1);
                                    opacity: 0.4;
                                }
                                100% {
                                    transform: scale(2.5);
                                    opacity: 0;
                                }
                            }
                            .hb-1 {
                                animation: hb-pulse 0.5s ease-out infinite;
                                animation-duration: 3s;
                                will-change: transform, opacity;
                                transform-origin: center;
                            }
                            .hb-2 {
                                animation: hb-pulse 0.5s ease-out infinite;
                                animation-delay: 1.2s;
                                animation-duration: 3s;
                                will-change: transform, opacity;
                                transform-origin: center;
                            }
                            .hb-3 {
                                animation: hb-pulse 0.5s ease-out infinite;
                                animation-delay: 1.85s;
                                animation-duration: 3s;
                                will-change: transform, opacity;
                                transform-origin: center;
                            }
                        `}),(0,s.jsx)(`svg`,{className:`hb-1`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})}),(0,s.jsx)(`svg`,{className:`hb-2`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})}),(0,s.jsx)(`svg`,{className:`hb-3`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})}),(0,s.jsx)(`svg`,{width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})})]}):i===`pulse`?(0,s.jsx)(f,{colorClass:t,tooltip:o,className:c}):(0,s.jsxs)(`div`,{"aria-label":o,"data-testid":`status-dot`,className:r(`relative block`,t,c),style:{width:24,height:24},children:[(0,s.jsx)(`style`,{children:`
                        @keyframes raindrop {
                            0% {
                                transform: scale(1);
                                opacity: 0.4;
                            }
                            100% {
                                transform: scale(2.5);
                                opacity: 0;
                            }
                        }
                        .raindrop {
                            animation: raindrop 1.5s ease-out infinite;
                            will-change: transform, opacity;
                            transform-origin: center;
                        }
                    `}),(0,s.jsx)(`svg`,{className:`raindrop`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})}),(0,s.jsx)(`svg`,{width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})})]}):n===`solid`&&e===`lg`?(0,s.jsx)(`svg`,{"aria-label":o,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,xmlns:`http://www.w3.org/2000/svg`,className:r(`block`,t,c),"data-testid":`status-dot`,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})}):n===`small`&&e===`lg`?(0,s.jsx)(`svg`,{"aria-label":o,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,xmlns:`http://www.w3.org/2000/svg`,className:r(`block`,t,c),"data-testid":`status-dot`,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`2`,fill:`currentColor`})}):(0,s.jsx)(a,{"aria-label":o,size:e,className:r(`block`,t,i===`none`?void 0:`animate-pulse`,c),"data-testid":`status-dot`}),f=({colorClass:e,tooltip:t,className:n})=>{let[i,a]=(0,o.useState)(!0);return(0,o.useEffect)(()=>{let e=setTimeout(()=>a(!1),2400);return()=>clearTimeout(e)},[]),(0,s.jsxs)(`div`,{"aria-label":t,"data-testid":`status-dot`,className:r(`relative block`,e,n),style:{width:24,height:24},children:[i&&(0,s.jsxs)(s.Fragment,{children:[(0,s.jsx)(`style`,{children:`
                            @keyframes pulse-ring {
                                0% {
                                    transform: scale(1);
                                    opacity: 0.4;
                                }
                                100% {
                                    transform: scale(2.5);
                                    opacity: 0;
                                }
                            }
                            .pulse-ring-svg {
                                transform-origin: center;
                                will-change: transform, opacity;
                                animation: pulse-ring 0.6s ease-out forwards;
                                opacity: 0;
                            }
                            .pulse-ring-svg-1 { animation-delay: 0s; }
                            .pulse-ring-svg-2 { animation-delay: 0.8s; }
                            .pulse-ring-svg-3 { animation-delay: 1.6s; }
                        `}),(0,s.jsx)(`svg`,{className:`pulse-ring-svg pulse-ring-svg-1`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})}),(0,s.jsx)(`svg`,{className:`pulse-ring-svg pulse-ring-svg-2`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})}),(0,s.jsx)(`svg`,{className:`pulse-ring-svg pulse-ring-svg-3`,width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})})]}),(0,s.jsx)(`svg`,{width:`24`,height:`24`,viewBox:`0 0 24 24`,fill:`none`,style:u,children:(0,s.jsx)(`circle`,{cx:`12`,cy:`12`,r:`4`,fill:`currentColor`})})]})};export{l as t};