#include "../config.h"

#ifdef HAVE_SECURITY_PAM_MODULES_H
#define PAM_SM_AUTH
#define PAM_SM_SESSION
#include <security/pam_modules.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <krb5.h>
#ifdef USE_KRB4
#include KRB4_DES_H
#include KRB4_KRB_H
#ifdef KRB4_KRB_ERR_H
#include KRB4_KRB_ERR_H
#endif
#endif

#include "conv.h"
#include "init.h"
#include "initopts.h"
#include "items.h"
#include "log.h"
#include "options.h"
#include "prompter.h"
#include "sly.h"
#include "stash.h"
#include "tokens.h"
#include "userinfo.h"
#include "v5.h"
#include "v4.h"

#ident "$Id$"

int
pam_sm_authenticate(pam_handle_t *pamh, int flags,
		    int argc, PAM_KRB5_MAYBE_CONST char **argv)
{
	PAM_KRB5_MAYBE_CONST char *user;
	krb5_context ctx;
	struct _pam_krb5_options *options;
	struct _pam_krb5_user_info *userinfo;
	struct _pam_krb5_stash *stash;
	krb5_get_init_creds_opt gic_options;
	int i, retval;
	char *password;

	/* Initialize Kerberos. */
	if (_pam_krb5_init_ctx(&ctx, argc, argv) != 0) {
		warn("error initializing Kerberos");
		return PAM_SERVICE_ERR;
	}

	/* Get the user's name. */
	i = pam_get_user(pamh, &user, NULL);
	if (i != PAM_SUCCESS) {
		warn("could not identify user name");
		krb5_free_context(ctx);
		return i;
	}

	/* Read our options. */
	options = _pam_krb5_options_init(pamh, argc, argv, ctx);
	if (options == NULL) {
		warn("error parsing options (shouldn't happen)");
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}
	if (options->debug) {
		debug("called to authenticate '%s'", user);
	}
	krb5_get_init_creds_opt_init(&gic_options);
	_pam_krb5_set_init_opts(ctx, &gic_options, options);

	/* Get information about the user and the user's principal name. */
	userinfo = _pam_krb5_user_info_init(ctx, user, options->realm,
					    options->user_check);
	if (userinfo == NULL) {
		warn("error getting information about '%s'", user);
		if (options->use_second_pass) {
			password = NULL;
			i = _pam_krb5_prompt_for(pamh, "Password: ", &password);
			if ((i == PAM_SUCCESS) &&
			    (flags & PAM_DISALLOW_NULL_AUTHTOK) &&
			    (strlen(password) == 0)) {
				warn("disallowing NULL authtok for '%s'", user);
				retval = PAM_AUTH_ERR;
				i = PAM_AUTH_ERR;
			}
			if (i == PAM_SUCCESS) {
				/* Save the password for the next module. */
				if (!_pam_krb5_has_item(pamh, PAM_AUTHTOK)) {
					pam_set_item(pamh, PAM_AUTHTOK, password);
				}
			}
		}
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}
	if (options->debug) {
		debug("authenticating '%s'", userinfo->unparsed_name);
	}

	/* Check the minimum UID argument. */
	if ((options->minimum_uid != -1) &&
	    (userinfo->uid < options->minimum_uid)) {
		if (options->debug) {
			debug("ignoring '%s' -- uid below minimum = %lu", user,
			      (unsigned long) options->minimum_uid);
		}
		_pam_krb5_user_info_free(ctx, userinfo);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_IGNORE;
	}

	/* Get the stash for this user. */
	stash = _pam_krb5_stash_get(pamh, userinfo);
	if (stash == NULL) {
		warn("error retrieving stash for '%s' (shouldn't happen)",
		     user);
		_pam_krb5_user_info_free(ctx, userinfo);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}

	/* Try with the stored password, if we've been told to do so. */
	retval = PAM_AUTH_ERR;
	if ((retval != PAM_SUCCESS) && (options->use_first_pass)) {
		i = _pam_krb5_get_item_text(pamh, PAM_AUTHTOK, &password);
		if ((i == PAM_SUCCESS) &&
		    (flags & PAM_DISALLOW_NULL_AUTHTOK) &&
		    (password != NULL) &&
		    (strlen(password) == 0)) {
			warn("disallowing NULL authtok for '%s'", user);
			retval = PAM_AUTH_ERR;
			i = PAM_AUTH_ERR;
		}
		if ((i == PAM_SUCCESS) && (password != NULL)) {
			if (options->debug) {
				debug("trying previously-entered password for "
				      "'%s'", user);
			}
			retval = v5_get_creds(ctx, pamh,
					      &stash->v5creds, userinfo,
					      options,
					      KRB5_TGS_NAME,
					      password, &gic_options,
					      &stash->v5result);
			if (options->debug) {
				debug("got result %d (%s)", stash->v5result,
				      v5_error_message(stash->v5result));
			}
		}
		if ((retval == PAM_SUCCESS) && (options->v4 == 1)) {
			v4_get_creds(ctx, pamh, stash, userinfo, options,
				     password, &i);
			if ((i != 0) && (options->debug)) {
				debug("error obtaining v4 creds: %d (%s)",
				      i, v5_error_message(i));
			}
		}
	}

	/* If that didn't work, ask for a new password and try again. */
	if ((retval != PAM_SUCCESS) && (options->use_second_pass)) {
		password = NULL;
		i = _pam_krb5_prompt_for(pamh, "Password: ", &password);
		if ((i == PAM_SUCCESS) &&
		    (flags & PAM_DISALLOW_NULL_AUTHTOK) &&
		    (password != NULL) &&
		    (strlen(password) == 0)) {
			warn("disallowing NULL authtok for '%s'", user);
			retval = PAM_AUTH_ERR;
			i = PAM_AUTH_ERR;
		}
		if ((i == PAM_SUCCESS) && (password != NULL)) {
			/* Save the password for the next module. */
			if (!_pam_krb5_has_item(pamh, PAM_AUTHTOK)) {
				pam_set_item(pamh, PAM_AUTHTOK, password);
			}
			/* Get creds. */
			if (options->debug) {
				debug("trying newly-entered password for "
				      "'%s'", user);
			}
			retval = v5_get_creds(ctx, pamh,
					      &stash->v5creds, userinfo,
					      options,
					      KRB5_TGS_NAME,
					      password, &gic_options,
					      &stash->v5result);
			if (options->debug) {
				debug("got result %d (%s)", stash->v5result,
				      v5_error_message(stash->v5result));
			}
		} else {
			warn("error reading password for '%s'", user);
		}
		if ((retval == PAM_SUCCESS) && (options->v4 == 1)) {
			v4_get_creds(ctx, pamh, stash, userinfo, options,
				     password, &i);
			if ((i != 0) && (options->debug)) {
				debug("error obtaining v4 creds: %d (%s)",
				      i, v5_error_message(i));
			}
			if (stash->v4present && (options->tokens == 1)) {
				v5_save(ctx, stash, userinfo, options, NULL);
				v4_save(ctx, stash, userinfo, options, NULL);
				tokens_obtain(options);
				v4_destroy(ctx, stash, options);
				v5_destroy(ctx, stash, options);
			}
		}
		if (password != NULL) {
			memset(password, '\0', strlen(password));
			free(password);
		}
	}

	if (retval == PAM_SUCCESS) {
		notice("authentication succeeds for '%s'", user);
	} else {
		notice("authentication fails for '%s' (%s)", user,
		       v5_error_message(stash->v5result));
	}

	/* Clean up. */
	_pam_krb5_options_free(pamh, ctx, options);
	_pam_krb5_user_info_free(ctx, userinfo);
	krb5_free_context(ctx);

	return retval;
}

int
pam_sm_setcred(pam_handle_t *pamh, int flags,
	       int argc, PAM_KRB5_MAYBE_CONST char **argv)
{
	if (flags & PAM_ESTABLISH_CRED) {
		return pam_sm_open_session(pamh, flags, argc, argv);
	}
	if (flags & (PAM_REINITIALIZE_CRED | PAM_REFRESH_CRED)) {
		return _pam_krb5_sly_maybe_refresh(pamh, flags, argc, argv);
	}
	if (flags & PAM_DELETE_CRED) {
		return pam_sm_close_session(pamh, flags, argc, argv);
	}
	warn("pam_setcred() called with no flags");
	return PAM_SERVICE_ERR;
}