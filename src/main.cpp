#include <QApplication>
#include <QCoreApplication>

#include "main_window.hpp"

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("ThermalSeek"));
  QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  MainWindow window;
  window.show();

  return application.exec();
}
