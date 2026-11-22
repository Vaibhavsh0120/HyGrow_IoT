ALTER TABLE background_jobs DROP CONSTRAINT background_jobs_kind_check;
ALTER TABLE background_jobs ADD CONSTRAINT background_jobs_kind_check CHECK (kind IN ('expire_otps','send_mail'));
CREATE TABLE mail_outbox (
  id uuid PRIMARY KEY REFERENCES background_jobs(id) ON DELETE CASCADE,
  user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  challenge_id uuid REFERENCES otp_challenges(id) ON DELETE CASCADE,
  payload text,
  sent_at timestamptz,
  created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX mail_challenge ON mail_outbox(challenge_id);
CREATE INDEX sessions_family ON sessions(family_id);
