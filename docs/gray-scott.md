# Gray-Scott demonstrator: specification

The demonstrator computes the Gray-Scott reaction-diffusion model and analyses
it while it runs. This page defines what is computed, precisely enough that two
implementations (the C++ pipeline and the Python reference in `python/`) agree
within a tolerance.

## Model

Two fields `U` and `V` on a `width × height` grid of cells, periodic in both
directions. Parameters (control message `JobStart`): feed `F`, kill `k`,
diffusion rates `Du`, `Dv`, time step `dt`, noise `seed`, number of `steps`,
snapshot interval `snapshotEvery`.

One step, for every cell `c` with neighbors north `n`, south `s`, west `w`,
east `e` (periodic), using the values of the previous step, evaluated in this
order:

```
lapU = (((U[n] + U[s]) + U[w]) + U[e]) - 4.0 * U[c]
lapV = (((V[n] + V[s]) + V[w]) + V[e]) - 4.0 * V[c]
uvv  = (U[c] * V[c]) * V[c]
U'[c] = U[c] + dt * ((Du * lapU - uvv) + F * (1.0 - U[c]))
V'[c] = V[c] + dt * ((Dv * lapV + uvv) - (F + k) * V[c])
```

Cell `(x, y)` is column `x`, row `y`; its index is `i = y * width + x`.
North is row `y - 1`, south row `y + 1`, west column `x - 1`, east `x + 1`.

## Initial condition

1. `U = 1`, `V = 0` everywhere.
2. In the centered square of side `s = max(1, min(width, height) / 8)` (integer
   division), from `x0 = (width - s) / 2`, `y0 = (height - s) / 2`:
   `U = 0.5`, `V = 0.25`.
3. Noise everywhere: `U += 0.02 * (r(2i) - 0.5)`, `V += 0.02 * (r(2i + 1) - 0.5)`,
   then both clamped to `[0, 1]`.

`r(j)` is a number in `[0, 1)` from the cell index alone (any implementation,
any split of the grid gives the same field), with 64-bit unsigned wrap-around:

```
z = seed + (j + 1) * 0x9E3779B97F4A7C15
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
z = (z ^ (z >> 27)) * 0x94D049BB133111EB
z =  z ^ (z >> 31)
r = (z >> 11) * 2^-53
```

## Measures

At every step that is a multiple of `snapshotEvery`, and after the last step
(step numbers count from 1):

- **Sums** `ΣU`, `ΣV` over all cells.
- **Histogram** of `V`: 64 bins over `[0, 1]`; a value `v` goes to bin
  `min(63, max(0, floor(v * 64)))`.

## Split of the work

- **Simulation group** (`simWorkers` workers): worker `i` of `S` owns rows
  `[floor(i * H / S), floor((i + 1) * H / S))`. Each step it needs the last row
  of the band above and the first row of the band below (periodic): the halo.
- **Analysis group** (`analysisWorkers` workers): the same rule with `A` parts.
  At a snapshot, every simulation worker sends to every analysis worker the
  rows of `V` the two bands share.
- Sums: each simulation worker adds its cells row by row, left to right; the
  group combines the partial sums in member order.
- Histograms: each analysis worker counts its rows; the counts are combined in
  a tree of pairs: `(0,1), (2,3), …`, then `(0,2), (4,6), …`, until worker 0
  holds the total.

## Comparison with the reference

Fields: `|x - ref| <= atol + rtol * |ref|`, with `rtol = 1e-9`, `atol = 1e-12`;
the largest difference and its cell are reported. Sums: relative difference
at most `1e-9`. Histograms: the L1 distance, summed over the bins, at most
`max(1, 0.001 * width * height)` (a value close to a bin edge may land in the
neighboring bin when fields differ in the last bits).
