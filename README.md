# FM Radio Radio Explorer
## Jacob Feenstra & Chun-Ho Chen

~Note this webpage is subject to change~

Historically, FM radio provided a dominating medium to
organically explore music, curated by the on-air disc jockey.
Radio has not enjoyed the popularity it has in the past, with
many music listeners opting for streaming services for their
discovery. What if we could have the best of both worlds?
Introducing our FM Radio Explorer, we devise an embedded
system that provides radio tuning capabilities, in addition to
providing exploratory information on the current track, all
from the same interface.

At a high level, we will use LastFM’s public-facing API to
query the song & artist name from the FM playback (using the
CC3200’s WiFi Card). LastFM offers a wide variety of meta-
data and points of musical exploration, which will be discussed
with higher granularity in Section III. The S10-S3 Univeral
Remote and IR Receiver, along with Lab 3’s send-receive
multi-tap texting, will write to the Digital Signal Processing
(DSP) radio module and play a selected FM broadband (for
example, 90.3 would correlate to 90.3 FM). An antenna will be
utilized to provide greater signal gain, and a peripheral speaker
will translate the FM radio to coherent audio. Our system
will enable a user to switch between different FM radios
seamlessly, and relevant information pulled from LastFM will
be indicated on the Adafruit OLED in different views, which
the user will be able to interact with. Note all of this will be
orchestrated with our Texas Instrument’s CC3200 LaunchPad.

## System Architecture Overview (First Draft)

<img src="reports/final_proposal/state_diagram.png" width="400" />

## User Interaction & Program Use (First Draft)

<img src="reports/final_proposal/system_implementation.png" width="400" />
