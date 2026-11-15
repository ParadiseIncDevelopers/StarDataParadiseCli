#include "../automation/sql_script.hpp"

#include <cstdlib>
#include <iostream>

namespace
{
void require(bool passed, const char* message)
{
    if (passed) return;
    std::cerr << message << '\n';
    std::exit(1);
}
}

int main()
{
    const auto basic = split_sql(
        "/* opening ; */ SELECT 'escaped '' quote ; value', \"a;b\"; -- trailing ;\n"
        "SELECT 'next;'; /* final comment ; */", true);
    require(basic.statements.size() == 2 && basic.remainder.empty() && !basic.incomplete, "quoted/comment delimiters");

    const auto routine = split_sql(
        "CREATE FUNCTION sample(x BIGINT) RETURN BIGINT AS DECLARE y BIGINT; BEGIN\n"
        "IF x > 0 THEN y := CASE WHEN x > 1 THEN 2 ELSE 1 END; ELSE y := 0; END IF;\n"
        "WHILE y < x LOOP y := y + 1; END LOOP;\n"
        "BEGIN y := y + 1; END; RETURN y; END;\n"
        "-- second statement\nSELECT sample(3);", true);
    require(routine.statements.size() == 2 && routine.remainder.empty() && !routine.incomplete, "routine block delimiters");

    const auto tx = split_sql("BEGIN; BEGIN WORK; BEGIN TRANSACTION; COMMIT; ", true);
    require(tx.statements.size() == 4 && !tx.incomplete, "transaction delimiters");

    const auto pending = split_sql("SELECT 1; BEGIN\n  x := 'abc;", false);
    require(pending.statements.size() == 1 && pending.incomplete && pending.remainder.starts_with(" BEGIN"), "partial REPL");
    const auto complete = split_sql(" BEGIN\n  x := 'abc;';\nEND;", false);
    require(complete.statements.size() == 1 && complete.remainder.empty(), "completed REPL block");

    const auto eof = split_sql("SELECT 1", true);
    require(eof.statements.size() == 1 && eof.remainder.empty(), "EOF SQL statement");
    const auto comment = split_sql("SELECT 1; /* open", true);
    require(comment.statements.size() == 1 && comment.incomplete, "unterminated comment");
    const auto declaration = split_sql("DECLARE x BIGINT;\nBEGIN x := 1;", true);
    require(declaration.statements.empty() && declaration.incomplete, "unterminated PL block");
    const auto create = split_sql("CREATE OR REPLACE PROCEDURE p() AS BEGIN RETURN; END; SELECT 1;", true);
    require(create.statements.size() == 2 && !create.incomplete, "replace routine");
}
