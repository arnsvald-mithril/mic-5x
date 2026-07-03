\# LTspice simulations

Two key simulations validate the analog dust-sensing front-end (TIA stage):

1. \*\*Transfer characteristic\*\* — DC sweep 0–30 µA photocurrent →

0\.3–3.3 V output. Confirms the TIA gain stage operating range

used in the error budget (see datasheet §5.4).

![TIA transfer](../../docs/images/TIA\_transfer\_characteristic.png)

1. \*\*Frequency response\*\* — two-pole filter, τ ≈ 1.8 ms,

−3 dB at ~90 Hz.

![TIA frequency response](../../docs/images/TIA\_frequency\_response.png)

Original `.asc` files were iterated directly and not preserved as

separate versions. Circuit parameters (Rf, Cf, LM358 config, BPW34

model) are fully specified in the datasheet and can be rebuilt from

those values if needed.
