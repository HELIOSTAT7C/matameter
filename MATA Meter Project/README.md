# MATA Meter

Transformer monitoring dashboard and ESP32 firmware.

## Dashboard

The Vercel entry point is `index.html`. It reads live telemetry from Supabase and uses GPS data from the MATA device. Run `supabase/schema-mata.sql` in the Supabase SQL editor before deploying.

## Firmware

The PlatformIO project is in `firmware/`. Copy `firmware/include/secrets.example.h` to `firmware/include/secrets.h`, fill in the Wi-Fi and Supabase values, then upload with PlatformIO.

The firmware uploads telemetry to Supabase every 10 seconds.

## Vercel

Import this repository into Vercel with the project root set to the repository root. No build command is required for the static dashboard.