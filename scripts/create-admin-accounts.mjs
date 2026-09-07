import { randomInt } from 'node:crypto';

const supabaseUrl = process.env.SUPABASE_URL || 'https://dyukzahnxtmusfksqdfn.supabase.co';
const serviceRoleKey = process.env.SUPABASE_SERVICE_ROLE_KEY;
const adminStart = Number(process.env.MATA_ADMIN_START || 1);
const adminCount = Number(process.env.MATA_ADMIN_COUNT || 5);

if (!serviceRoleKey) {
    throw new Error('Set SUPABASE_SERVICE_ROLE_KEY before creating admin accounts.');
}

if (!Number.isInteger(adminStart) || adminStart < 1 || !Number.isInteger(adminCount) || adminCount < 1) {
    throw new Error('MATA_ADMIN_START and MATA_ADMIN_COUNT must be positive integers.');
}

const adminUsers = Array.from({ length: adminCount }, (_, index) => {
    const number = adminStart + index;
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

const usersResponse = await supabaseRequest('/auth/v1/admin/users?per_page=1000');
const existingUsers = new Map((usersResponse.users || []).map(user => [user.email, user]));

for (const admin of adminUsers) {
    const userPayload = {
        email: admin.email,
        password: admin.pin,
        email_confirm: true,
        user_metadata: {
            display_name: admin.display_name,
            role: 'admin'
        },
        app_metadata: { role: 'admin' }
    };
    const existingUser = existingUsers.get(admin.email);
    const user = existingUser
        ? await supabaseRequest(`/auth/v1/admin/users/${existingUser.id}`, {
            method: 'PUT',
            body: JSON.stringify(userPayload)
        })
        : await supabaseRequest('/auth/v1/admin/users', {
            method: 'POST',
            body: JSON.stringify(userPayload)
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