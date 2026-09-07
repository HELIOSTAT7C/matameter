# matameter
Upgrade for our thesis project on Transformer Load Management

Transformer monitoring dashboard and ESP32 firmware.

## Dashboard

The Vercel entry point is `index.html`. It reads live telemetry from Supabase and uses GPS data from the MATA device. Run `supabase/schema-mata.sql` in the Supabase SQL editor before deploying.

## Firmware

The PlatformIO project is in `firmware/`. Copy `firmware/include/secrets.example.h` to `firmware/include/secrets.h`, fill in the Wi-Fi and Supabase values, then upload with PlatformIO.

The firmware uploads telemetry to Supabase every 10 seconds.

## Supabase authentication

The dashboard uses Supabase Auth. Run `supabase/schema-mata.sql` in the Supabase
SQL editor before deploying so telemetry reads require an authorized admin while
anonymous firmware inserts remain available.

Create the five admin accounts from a trusted machine with Node.js 18 or newer:

```powershell
$env:SUPABASE_SERVICE_ROLE_KEY = '<paste the service-role key temporarily>'
node scripts/create-admin-accounts.mjs
Remove-Item Env:SUPABASE_SERVICE_ROLE_KEY
```

The script creates valid Auth login emails `mata+admin1@csu-c.ee` through
`mata+admin5@csu-c.ee`. Each account receives a different cryptographically
generated six-digit PIN and the requested display name `mata@csu-c.ee_1` through
`mata@csu-c.ee_5`. Save the one-time command output securely. Never commit the
service-role key or PINs, and never place the service-role key in the dashboard.

To add only admin number 6 later without changing the existing accounts:

```powershell
$env:MATA_ADMIN_START = "6"
$env:MATA_ADMIN_COUNT = "1"
node scripts/create-admin-accounts.mjs
Remove-Item Env:MATA_ADMIN_START, Env:MATA_ADMIN_COUNT
```

If the Supabase project enforces a password minimum longer than six characters,
set the Auth password minimum to six characters before running the provisioning
script, since the requested PINs are six digits.

## Vercel

Import this repository into Vercel with the project root set to the repository root. No build command is required for the static dashboard.
