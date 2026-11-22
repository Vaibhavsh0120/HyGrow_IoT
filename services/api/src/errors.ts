export class ApiError extends Error {
  constructor(public readonly statusCode: number, public readonly code: string, message: string, public readonly retryAfterSeconds?: number) { super(message); }
}
