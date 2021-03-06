#include "engine.h"

#include <QMetaType>
#include <QString>
#include <QByteArray>

namespace qpjsua {

static Engine *instance = 0;

Engine::Engine(QObject *parent) : QObject(parent), error(), status(PJ_SUCCESS)
{
    qRegisterMetaType<CallInfo>("qpjsua::CallInfo");
    qRegisterMetaType<AccountInfo>("qpjsua::AccountInfo");
}

Engine::~Engine()
{
    pjsua_call_hangup_all();
    pjsua_destroy();
    instance = 0;
}

Engine::Builder Engine::build()
{
    return Builder();
}

Engine::Builder::Builder()
    : mediaConfiguration(MediaConfiguration::build()),
      loggingConfiguration(LoggingConfiguration::build()),
      transportConfiguration(TransportConfiguration::build())
{}

Engine::Builder &Engine::Builder::withLoggingConfiguration(const LoggingConfiguration &aLoggingConfiguration)
{
    loggingConfiguration = aLoggingConfiguration;
    return *this;
}

Engine::Builder &Engine::Builder::withMediaConfiguration(const MediaConfiguration &aMediaConfiguration)
{
    mediaConfiguration = aMediaConfiguration;
    return *this;
}

Engine::Builder &Engine::Builder::withTransportConfiguration(const TransportConfiguration &aTransportConfiguration)
{
    transportConfiguration = aTransportConfiguration;
    return *this;
}

Engine *Engine::Builder::create(QObject *parent) const
{
    qRegisterMetaType<pjsua_acc_id>("pjsua_acc_id");
    qRegisterMetaType<pjsua_call_id>("pjsua_call_id");

    Engine *engine = new Engine(parent);

    engine->status = pjsua_create();
    if(engine->checkStatus("pjsua create") == false) {
        return engine;
    }

    pjsua_config config;
    pjsua_config_default(&config);

    config.cb.on_incoming_call = &on_incoming_call_wrapper;
    config.cb.on_call_state = &on_call_state_wrapper;
    config.cb.on_call_media_state = &on_call_media_state_wrapper;
    config.cb.on_reg_started = &on_reg_started_wrapper;
    config.cb.on_transport_state = &on_transport_state_wrapper;

    pjsua_logging_config loggingConfig;
    pjsua_logging_config_default(&loggingConfig);
    loggingConfig.decor = loggingConfig.decor & ~PJ_LOG_HAS_NEWLINE;
    loggingConfig.console_level = loggingConfiguration.getConsoleLevel();
    loggingConfig.cb = &logger_callback_wrapper;
    const QObject *receiver = loggingConfiguration.getReceiver();
    if(receiver) {
        receiver->connect(engine, SIGNAL(log(int,QString)), loggingConfiguration.getMember(), Qt::QueuedConnection);
    }
    pjsua_media_config mediaConfig;
    pjsua_media_config_default(&mediaConfig);

    engine->status = pjsua_init(&config, &loggingConfig, &mediaConfig);
    if(engine->checkStatus("pjsua init") == false) {
        return engine;
    }

    pjsua_transport_config transportConfig;
    pjsua_transport_config_default(&transportConfig);

    transportConfig.port = transportConfiguration.getPort();

    engine->status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &transportConfig, NULL);
    if(engine->checkStatus("pjsua transport") == false) {
        return engine;
    }

    engine->status = pjsua_start();
    if(engine->checkStatus("pjsual start") == false) {
        return engine;
    }
    instance = engine;
    return engine;
}

pjsua_acc_id Engine::addAccount(AccountConfiguration *anAccountConfiguration)
{
    pjsua_acc_config accountConfig;
    pjsua_acc_config_default(&accountConfig);

    accountConfig.id = pj_str(anAccountConfiguration->sipUrl.data());
    accountConfig.reg_uri = pj_str(anAccountConfiguration->registrationUri.data());
    accountConfig.rtp_cfg.port = anAccountConfiguration->port;
    if(anAccountConfiguration->allowRewrite) {
        accountConfig.allow_contact_rewrite = PJ_TRUE;
    } else {
        accountConfig.allow_contact_rewrite = PJ_FALSE;
    }
    accountConfig.proxy_cnt = anAccountConfiguration->proxys.size();
    for(int i = 0; i < anAccountConfiguration->proxys.size(); ++i) {
        QByteArray *proxy = anAccountConfiguration->proxys.at(i);
        accountConfig.proxy[i] = pj_str(proxy->data());
    }

    accountConfig.cred_count = anAccountConfiguration->credentials.size();
    for(int i = 0; i < anAccountConfiguration->credentials.size(); ++i) {
        AccountCredential *credential = anAccountConfiguration->credentials.at(i);
        accountConfig.cred_info[i].realm = pj_str(credential->realm.data());
        accountConfig.cred_info[i].scheme = pj_str(credential->scheme.data());
        accountConfig.cred_info[i].username = pj_str(credential->username.data());
        accountConfig.cred_info[i].data_type = credential->type;
        accountConfig.cred_info[i].data = pj_str(credential->password.data());
    }
    pjsua_acc_id account_id;

    status = pjsua_acc_add(&accountConfig, PJ_TRUE, &account_id);
    if(checkStatus("Add account") == false) {
        return -1;
    }
    return account_id;
}

bool Engine::isValid() const
{
    return status == PJ_SUCCESS;
}

PjError Engine::lastError() const
{
    return error;
}

bool Engine::checkStatus(const QString &aMessage)
{
    bool ret = true;
    if(status != PJ_SUCCESS) {
        pjsua_destroy();
        error.setStatus(status);
        error.setMessage(QString("%1 failed.").arg(aMessage));
        ret = false;
    }
    return ret;
}

void Engine::logger_callback_wrapper(int level, const char *data, int len)
{
    if(instance) {
        instance->logger_callback(level, data, len);
    }
}

void Engine::logger_callback(int level, const char *data, int len)
{
    Q_UNUSED(len);
    emit log(level, data);
}

void Engine::on_call_state_wrapper(pjsua_call_id call_id, pjsip_event *event)
{
    if(instance) {
        instance->on_call_state(call_id, event);
    }
}

void Engine::on_call_state(pjsua_call_id call_id, pjsip_event *event)
{
    Q_UNUSED(event);

    pjsua_call_info call_info;
    pjsua_call_get_info(call_id, &call_info);

    emit callState(CallInfo(call_info));
}

void Engine::on_incoming_call_wrapper(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata)
{
    if(instance) {
        instance->on_incoming_call(acc_id, call_id, rdata);
    }
}

void Engine::on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata)
{
    Q_UNUSED(rdata);

    pjsua_call_info call_info;
    pjsua_call_get_info(call_id, &call_info);

    pjsua_acc_info acc_info;
    pjsua_acc_get_info(acc_id, &acc_info);

    emit incomingCall(AccountInfo(acc_info), CallInfo(call_info));
}

void Engine::on_call_media_state_wrapper(pjsua_call_id call_id)
{
    if(instance) {
        instance->on_call_media_state(call_id);
    }
}

void Engine::on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info call_info;
    pjsua_call_get_info(call_id, &call_info);

    emit callMediaState(CallInfo(call_info));
}

void Engine::on_reg_started_wrapper(pjsua_acc_id acc_id, pj_bool_t renew)
{
    if(instance) {
        instance->on_reg_started(acc_id, renew);
    }
}

void Engine::on_reg_started(pjsua_acc_id acc_id, pj_bool_t renew)
{
    bool _renew = false;
    if(renew == PJ_TRUE) {
        _renew = true;
    }

    pjsua_acc_info acc_info;
    pjsua_acc_get_info(acc_id, &acc_info);

    emit regStarted(AccountInfo(acc_info), _renew);
}

void Engine::on_transport_state_wrapper(pjsip_transport *tp, pjsip_transport_state state, const pjsip_transport_state_info *info)
{
}

} // namespace qpjsua
