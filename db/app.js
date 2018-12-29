const status_mapping = {
    'свободны': 0,
    'заняты': 1,
    'всё сложно': 2
};

const accounts = [];
const interests = [];
const premium = [];
const likes = [];

const stream = require('JSONStream').parse('accounts.*');
stream.on('data', account => {
    accounts.push({
        id: account.id,
        email: account.email,
        fname: account.hasOwnProperty('fname') ? account.fname : null,
        sname: account.hasOwnProperty('sname') ? account.sname : null,
        phone: account.hasOwnProperty('phone') ? account.phone : null,
        sex: account.sex === 'm' ? 1 : 0,
        birth: account.birth,
        country: account.hasOwnProperty('country') ? account.country : null,
        city: account.hasOwnProperty('city') ? account.city : null,
        joined: account.joined,
        status: status_mapping[account.status]
    });

    if (account.hasOwnProperty('interests')) {
        account.interests.forEach(text => {
            interests.push({
                account_id: account.id,
                interest: text
            });
        });
    }

    if (account.hasOwnProperty('likes')) {
        account.likes.forEach(value => {
            likes.push({
                liker_id: account.id,
                likee_id: value.id,
                ts: value.ts
            });
        });
    }

    if (account.hasOwnProperty('premium')) {
        premium.push({
            account_id: account.id,
            start: account.premium.start,
            finish: account.premium.finish,
        })
    };
});

const file = require('fs').createReadStream('accounts_1.json');

const on_finish = () => {
    const Database = require('better-sqlite3');
    const db = new Database('db.db');

    const accounts_insert = db.prepare('INSERT INTO accounts VALUES (@id, @email, @fname, @sname, @phone, @sex, @birth, @country, @city, @joined, @status)');
    const interests_insert = db.prepare('INSERT INTO account_interests VALUES (@account_id, @interest)');
    const likes_insert = db.prepare('INSERT INTO likes VALUES (@liker_id, @likee_id, @ts)');
    const premium_insert = db.prepare('INSERT INTO premium VALUES (@account_id, @start, @finish)');

    db.transaction(() => {
        accounts.forEach(account => accounts_insert.run(account));
        interests.forEach(interest => interests_insert.run(interest));
        likes.forEach(like => likes_insert.run(like));
        premium.forEach(prem => premium_insert.run(prem));
    })();

    db.close();
};
stream.on('finish', on_finish);
stream.on('close', on_finish);


file.pipe(stream);