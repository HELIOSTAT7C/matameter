import { randomInt } from 'node:crypto';

const supabaseUrl = process.env.SUPABASE_URL || 'https://dyukzahnxtmusfksqdfn.supabase.co';
const serviceRoleKey = process.env.SUPABASE_SERVICE_ROLE_KEY;

if (!serviceRoleKey) {
    throw new Error('Set SUPABASE_SERVICE_ROLE_KEY before creating admin accounts.');
}

const adminUsers = Array.from({ length: 5 }, (_, index) => {
    const number = index + 1;
    return {
        email: `mata+admin${number}@csu-c.ee`,
        display_name: `mata@csu-c.ee_${number}`,
        pin: String(randomInt(100000, 1000000))
    };
});

async function supabaseRequest(path, options = {}) {
    const response = await fetch(`${supabaseUrl}${path}`, {
        ...options,
        headers: {
            apikey: serviceRoleKey,
            Authorization: `Bearer ${serviceRoleKey}`,
            'Content-Type': 'application/json',
            ...options.headers
        }
    });
    const body = await response.json().catch(() => null);
    if (!response.ok) {
        throw new Error(`${response.status}: ${JSON.stringify(body)}`);
    }
    return body;
}

await supabaseRequest('/rest/v1/admin_profiles?select=user_id&limit=1');

for (const admin of adminUsers) {
    const user = await supabaseRequest('/auth/v1/admin/users', {
        method: 'POST',
        body: JSON.stringify({
            email: admin.email,
            password: admin.pin,
            email_confirm: true,
            user_metadata: {
                display_name: admin.display_name,
                role: 'admin'
            },
            app_metadata: { role: 'admin' }
        })
    });

    await supabaseRequest('/rest/v1/admin_profiles', {
        method: 'POST',
        headers: { Prefer: 'resolution=merge-duplicates,return=minimal' },
        body: JSON.stringify({
            user_id: user.id,
            email: admin.email,
            display_name: admin.display_name,
            role: 'admin'
        })
    });
}

console.log('Created Supabase Auth admin accounts. Store these PINs securely; they are not saved by this script.');
for (const admin of adminUsers) {
    console.log(`${admin.display_name} | login: ${admin.email} | PIN: ${admin.pin}`);
}