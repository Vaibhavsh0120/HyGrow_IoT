-- A mailbox verification authorizes only the credentials chosen in that flow.
ALTER TABLE otp_challenges ADD COLUMN pending_password_hash text;
ALTER TABLE otp_challenges ADD COLUMN pending_display_name text;
