#pragma once

// Results that every kind of document shares.

enum class OpenResult { Ok, NeedPassword, WrongPassword, Failed };

enum class PrintOutcome { Printed, Cancelled, NotAllowed, Failed };
