#include "queryexecutorcolumns.h"
#include "common/utils_sql.h"
#include "parser/parser.h"
#include "parser/parsererror.h"
#include <QDebug>

// TODO need to test if attach name resolving works here

bool QueryExecutorColumns::exec()
{
    SqliteSelectPtr select = getSelect();
    if (!select || select->explain)
    {
        context->editionForbiddenReasons << QueryExecutor::EditionForbiddenReason::NOT_A_SELECT;
        return true;
    }

    // Resolving result columns of the select
    SelectResolver resolver(db, queryExecutor->getOriginalQuery(), context->dbNameToAttach);
    resolver.resolveMultiCore = true;
    QList<SelectResolver::Column> columns = resolver.resolve(select.data()).first();

    if (resolver.hasErrors())
    {
        qWarning() << "SelectResolver could not resolve the SELECT properly:" << resolver.getErrors().join("\n");
        return false;
    }

    if (columns.size() == 0)
    {
        qWarning() << "SelectResolver could not resolve any column. Probably wrong table name entered by user, or something like that.";
        return false;
    }

    // Deleting old result columns and defining new ones
    SqliteSelect::Core* core = select->coreSelects.first();
    for (SqliteSelect::Core::ResultColumn* resCol : core->resultColumns)
        delete resCol;

    core->resultColumns.clear();

    // Count total rowId columns
    for (const QueryExecutor::ResultRowIdColumnPtr& rowIdCol : context->rowIdColumns)
        rowIdColNames += rowIdCol->queryExecutorAliasToColumn.keys();

    // Defining result columns
    QueryExecutor::ResultColumnPtr resultColumn;
    SqliteSelect::Core::ResultColumn* resultColumnForSelect = nullptr;
    bool rowIdColumn = false;
    int i = 0;
    for (const SelectResolver::Column& col : columns)
    {
        // Convert column to QueryExecutor result column
        resultColumn = getResultColumn(col);

        // Adding new result column to the query
        rowIdColumn = isRowIdColumn(col.alias);
        if (rowIdColumn && col.alias.contains(":"))
            continue; // duplicate ROWID column provided by SelectResolver. See isRowIdColumn() for details.

        resultColumnForSelect = getResultColumnForSelect(resultColumn, col);
        if (!resultColumnForSelect)
            return false;

        resultColumnForSelect->setParent(core);
        core->resultColumns << resultColumnForSelect;

        if (!rowIdColumn)
            context->resultColumns << resultColumn; // store it in context for later usage by any step

        i++;
    }

    qDebug() << "before: " << context->processedQuery;
    // Update query
    select->rebuildTokens();
    wrapWithAliasedColumns(select.data());
    updateQueries();

    qDebug() << "after:  " << context->processedQuery;

    return true;
}

QueryExecutor::ResultColumnPtr QueryExecutorColumns::getResultColumn(const SelectResolver::Column &resolvedColumn)
{
    QueryExecutor::ResultColumnPtr resultColumn = QueryExecutor::ResultColumnPtr::create();
    if (resolvedColumn.type == SelectResolver::Column::OTHER)
    {
        resultColumn->editionForbiddenReasons << QueryExecutor::ColumnEditionForbiddenReason::EXPRESSION;
        resultColumn->displayName = resolvedColumn.displayName;
        resultColumn->column = resolvedColumn.column;
        resultColumn->alias = resolvedColumn.alias;
        resultColumn->expression = true;
    }
    else
    {
        if (isSystemTable(resolvedColumn.table))
            resultColumn->editionForbiddenReasons << QueryExecutor::ColumnEditionForbiddenReason::SYSTEM_TABLE;

        if (resolvedColumn.flags & SelectResolver::FROM_COMPOUND_SELECT)
            resultColumn->editionForbiddenReasons << QueryExecutor::ColumnEditionForbiddenReason::COMPOUND_SELECT;

        if (resolvedColumn.flags & SelectResolver::FROM_GROUPED_SELECT)
            resultColumn->editionForbiddenReasons << QueryExecutor::ColumnEditionForbiddenReason::GROUPED_RESULTS;

        if (resolvedColumn.flags & SelectResolver::FROM_DISTINCT_SELECT)
            resultColumn->editionForbiddenReasons << QueryExecutor::ColumnEditionForbiddenReason::DISTINCT_RESULTS;

        resultColumn->database = resolvedColumn.originalDatabase;
        resultColumn->table = resolvedColumn.table;
        resultColumn->column = resolvedColumn.column;
        resultColumn->tableAlias = resolvedColumn.tableAlias;
        resultColumn->alias = resolvedColumn.alias;
        resultColumn->displayName = resolvedColumn.displayName;
    }

    if (isRowIdColumnAlias(resultColumn->alias))
    {
        resultColumn->queryExecutorAlias = resultColumn->alias;
    }
    else
    {
        resultColumn->queryExecutorAlias = getNextColName();
    }
    return resultColumn;
}

SqliteSelect::Core::ResultColumn* QueryExecutorColumns::getResultColumnForSelect(const QueryExecutor::ResultColumnPtr& resultColumn, const SelectResolver::Column& col)
{
    SqliteSelect::Core::ResultColumn* selectResultColumn = new SqliteSelect::Core::ResultColumn();

    QString colString = resultColumn->column;
    if (col.aliasDefinedInSubQuery) // #2931
        colString = col.alias;

    if (!resultColumn->expression)
        colString = wrapObjIfNeeded(colString, dialect);

    Parser parser(dialect);
    SqliteExpr* expr = parser.parseExpr(colString);
    if (!expr)
    {
        qWarning() << "Could not parse result column expr:" << colString;
        if (parser.getErrors().size() > 0)
            qWarning() << "The error was:" << parser.getErrors().first()->getFrom() << ":" << parser.getErrors().first()->getMessage();

        return nullptr;
    }

    expr->setParent(selectResultColumn);
    selectResultColumn->expr = expr;

    if (!(col.flags & SelectResolver::FROM_ANONYMOUS_SELECT)) // anonymous subselect will result in no prefixes for result column
    {
        if (!resultColumn->tableAlias.isNull())
        {
            selectResultColumn->expr->table = resultColumn->tableAlias;
        }
        else if (!resultColumn->table.isNull())
        {
            if (!resultColumn->database.isNull())
            {
                if (context->dbNameToAttach.containsLeft(resultColumn->database, Qt::CaseInsensitive))
                    selectResultColumn->expr->database = context->dbNameToAttach.valueByLeft(resultColumn->database, Qt::CaseInsensitive);
                else
                    selectResultColumn->expr->database = resultColumn->database;
            }

            selectResultColumn->expr->table = resultColumn->table;
        }
    }

    selectResultColumn->asKw = true;
    if (!col.alias.isNull())
        selectResultColumn->alias = col.alias;
    else
        selectResultColumn->alias = resultColumn->queryExecutorAlias;

    return selectResultColumn;
}

QString QueryExecutorColumns::resolveAttachedDatabases(const QString &dbName)
{
    if (context->dbNameToAttach.containsRight(dbName, Qt::CaseInsensitive))
        return context->dbNameToAttach.valueByRight(dbName, Qt::CaseInsensitive);

    return dbName;
}

bool QueryExecutorColumns::isRowIdColumnAlias(const QString& alias)
{
    foreach (QueryExecutor::ResultRowIdColumnPtr rowIdColumn, context->rowIdColumns)
    {
        if (rowIdColumn->queryExecutorAliasToColumn.keys().contains(alias))
            return true;
    }
    return false;
}

void QueryExecutorColumns::wrapWithAliasedColumns(SqliteSelect* select)
{
    // Wrap everything in a surrounding SELECT and given query executor alias to all columns this time
    TokenList sepTokens;
    sepTokens << TokenPtr::create(Token::OPERATOR, ",") << TokenPtr::create(Token::SPACE, " ");

    bool first = true;
    TokenList outerColumns;
    for (const QueryExecutor::ResultRowIdColumnPtr& rowIdColumn : context->rowIdColumns)
    {
        for (const QString& alias : rowIdColumn->queryExecutorAliasToColumn.keys())
        {
            if (!first)
                outerColumns += sepTokens;

            outerColumns << TokenPtr::create(Token::OTHER, alias);
            first = false;
        }
    }

    QStringList columnNamesUsed;
    QString baseColName;
    QString colName;
    static_qstring(colNameTpl, "%1:%2");
    for (const QueryExecutor::ResultColumnPtr& resCol : context->resultColumns)
    {
        if (!first)
            outerColumns += sepTokens;

        // If alias was given, we use it. If it was anything but expression, we also use its display name,
        // because it's explicit column (no matter if from table, or table alias).
        baseColName = QString();
        if (!resCol->queryExecutorAlias.isNull())
            baseColName = resCol->alias;
        else if (!resCol->expression)
            baseColName = resCol->column;

        if (!baseColName.isNull())
        {
            colName = baseColName;
            for (int i = 1; columnNamesUsed.contains(colName, Qt::CaseInsensitive); i++)
                colName = colNameTpl.arg(resCol->column, QString::number(i));

            columnNamesUsed << colName;
            outerColumns << TokenPtr::create(Token::OTHER, wrapObjIfNeeded(colName, dialect));
            outerColumns << TokenPtr::create(Token::SPACE, " ");
            outerColumns << TokenPtr::create(Token::KEYWORD, "AS");
            outerColumns << TokenPtr::create(Token::SPACE, " ");
        }
        outerColumns << TokenPtr::create(Token::OTHER, resCol->queryExecutorAlias);
        first = false;
    }

    //QString t = outerColumns.detokenize(); // keeping it for debug purposes
    select->tokens = wrapSelect(select->tokens, outerColumns);
}

bool QueryExecutorColumns::isRowIdColumn(const QString& columnAlias)
{
    // In case of "SELECT * FROM (SELECT * FROM test);" the SelectResolver will return ROWID columns twice for each table listed,
    // because ROWID columns are recurrently handled by QueryExecutorAddRowIds step. We need to identify such columns and make them unique
    // in the final query.
    // Currently all columns have QueryExecutor aliased names, so we can assume they have unified alias name in form ResCol_N.
    // If SelectResolver returns any column like ResCol_N:X, then it means that the column is result of the query like above.
    // Note, that this assumption is correct for RowId columns. There can be columns aliased by user and those aliases won't be unified.
    QString aliasOnly = columnAlias;
    if (aliasOnly.contains(":"))
        aliasOnly = aliasOnly.left(aliasOnly.indexOf(":"));

    return rowIdColNames.contains(aliasOnly);
}
