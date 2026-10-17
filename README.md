# ptrans-rgb
Display public transport data in Vienna and what's currently playing on Spotify from data provided by [ptrans-data](https://github.com/lerchl/ptrans-data) configured through [ptrans-ui](https://github.com/lerchl/ptrans-ui).

## Electrical setup
```mermaid
flowchart LR
    AC["Mains 230V<br/>3x0.75mm², Schuko"] --> PSU["5V 20A PSU<br/>(L, N, earth)"]
    PSU -- "+5V (2.5mm²)" --> WP["Wago +5V"]
    PSU -- "GND (2.5mm²)" --> WG["Wago GND"]

    WP --> SP["Panel power splitter"]
    WG --> SP

    WP -- "red" --> PI["Raspberry Pi Zero 2 W<br/>(open-end micro-USB cable)"]
    WG -- "black" --> PI

    subgraph PANELS["HUB75 panels (daisy chain)"]
        direction TB
        M1["Panel 1<br/>IN"] -. "OUT → IN" .-> M2["Panel 2"]
        M2 -. "OUT → IN" .-> M3["Panel 3"]
    end

    SP -- "5V + GND to each panel" --> PANELS
    PI -. "HUB75 ribbon to Panel 1 IN:<br/>GPIO signals + GND" .-> PANELS
```
