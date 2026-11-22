CREATE TABLE background_jobs (
  id uuid PRIMARY KEY,
  kind text NOT NULL CHECK (kind IN ('expire_otps')),
  status text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending','completed','failed')),
  attempts integer NOT NULL DEFAULT 0,
  last_error_code text,
  completed_at timestamptz,
  created_at timestamptz NOT NULL DEFAULT now(),
  updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX jobs_pending ON background_jobs(created_at) WHERE status = 'pending';
