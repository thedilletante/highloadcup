DROP TABLE IF EXISTS accounts;
CREATE TABLE accounts (
    id INTEGER,
    email TEXT NOT NULL,
    fname TEXT,
    sname TEXT,
    phone TEXT,
    sex INTEGER,
    birth INTEGER,
    country TEXT,
    city TEXT,
    joined INTEGER,
    status INTEGER
);

DROP INDEX IF EXISTS id_index;
CREATE UNIQUE INDEX id_index ON accounts ( id );

DROP INDEX IF EXISTS id_index;
CREATE UNIQUE INDEX email_index ON accounts ( email );

DROP INDEX IF EXISTS fname_index;
CREATE INDEX fname_index ON accounts ( fname );

DROP INDEX IF EXISTS sname_index;
CREATE INDEX sname_index ON accounts ( sname );

DROP INDEX IF EXISTS phone_index;
CREATE INDEX phone_index ON accounts ( phone );

DROP INDEX IF EXISTS sex_index;
CREATE INDEX sex_index ON accounts ( sex );

DROP INDEX IF EXISTS birth_index;
CREATE INDEX birth_index ON accounts ( birth );

DROP INDEX IF EXISTS country_index;
CREATE INDEX country_index ON accounts ( country );

DROP INDEX IF EXISTS city_index;
CREATE INDEX city_index ON accounts ( city );

DROP INDEX IF EXISTS joined_index;
CREATE INDEX joined_index ON accounts ( joined );

DROP INDEX IF EXISTS status_index;
CREATE INDEX status_index ON accounts ( status );

DROP TABLE IF EXISTS account_interests;
CREATE TABLE account_interests (
    account_id INTEGER,
    interest TEXT NOT NULL
);

DROP INDEX IF EXISTS account_interest_index;
CREATE INDEX account_interest_index ON account_interests ( account_id );

DROP INDEX IF EXISTS interest_index;
CREATE INDEX interest_index ON account_interests ( interest );

DROP TABLE IF EXISTS likes;
CREATE TABLE likes (
    liker_id INTEGER,
    likee_id INTEGER,
    ts INT
);

DROP INDEX IF EXISTS likes_index;
CREATE INDEX likes_index ON likes ( liker_id, likee_id );

DROP TABLE IF EXISTS premium;
CREATE TABLE premium (
    account_id INTEGER PRIMARY KEY,
    start INTEGER,
    finish INTEGER
);