#ifndef CLI_H
#define CLI_H

#include "db/db.h"
#include <QObject>
#include <QTextStream>
#include <QStringList>

class QThread;
class QFile;
class DbManager;
class CliCommand;

class CLI : public QObject
{
    Q_OBJECT

    public:
        explicit CLI(QObject *parent = 0);
        ~CLI();

        void start();
        void setCurrentDb(Db* db);
        Db* getCurrentDb();
        void exit();

    private:
        void println(const QString& msg);
        void waitForExecution();
        bool isComplete(const QString& contents) const;

        DbManager* dbManager;
        QThread* thread;
        Db* currentDb = nullptr;
        bool executionFinished = false;
        bool doExit = false;

    signals:
        void execCommand(CliCommand* cmd, QStringList args);

    public slots:
        void doWork();
        void done();
        void executionComplete();
};

#endif // CLI_H
